#!/usr/bin/env python3
"""
Falcon — generate a scrolling-checkerboard Motion-JPEG frame set for the
ESP32 OV519/OV530 (Xbox Video Camera) emulator.

The OV519 bridge's on-chip JPEG engine streams baseline MJPEG at 320x240. For
Milestone 1 we do not run a JPEG encoder on the ESP32; instead we pre-encode a
short loop of scrolling-checkerboard frames here and cycle them at runtime to
show motion. Each frame is a complete baseline JPEG (DQT+DHT included) so any
decoder -- a v4l2 viewer over gspca_ov519, or the Xbox-side picojpeg -- can
decode it standalone.

Output: a C header with each frame as a byte array plus a {data,len} table.

Usage:
    python tools/gen_checkerboard_jpegs.py \
        --out main/checkerboard_jpegs.h [--frames 16] [--square 40] \
        [--quality 75] [--dump-dir out_jpegs]
"""
import argparse
import os
from PIL import Image, ImageDraw

W, H = 320, 240


# SMPTE-style vertical color bars: JPEG-friendly (smooth, low high-frequency
# content, no ringing) and the colours make any decode/chroma error obvious.
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def make_frame(phase, frames, square):
    """Vertical colour bars with a moving white marker bar, so the image is
    clean (JPEG-friendly) yet still animated to prove a live stream."""
    img = Image.new("RGB", (W, H))
    px = img.load()
    bar_w = W // len(BARS)
    marker_x = int(phase * (W - 8) / max(1, frames))  # sweeps left->right
    for y in range(H):
        for x in range(W):
            if marker_x <= x < marker_x + 8:
                px[x, y] = (255, 255, 255)          # moving white marker
            else:
                px[x, y] = BARS[min(x // bar_w, len(BARS) - 1)]
    return img


def _ping(t):
    """triangle wave 0->1->0 for constant-speed ping-pong; t in [0,1)."""
    t = (t * 2.0) % 2.0
    return t if t <= 1.0 else 2.0 - t


def animate_frame(base, p, frames):
    """base test-card with a Knight-Rider red scanner sweeping L<->R in the bottom
       centre (live motion)."""
    img = base.copy()
    d = ImageDraw.Draw(img)
    bar_w = 130                 # scanner span (compact -> small per-frame step = smooth)
    x0 = (W - bar_w) // 2       # centred horizontally
    y0 = H - 30                 # bottom
    bh = 16                     # bar height
    glow = 30.0                 # glow half-width (px) -> the trailing fade
    cx = x0 + int(_ping(p / float(frames)) * bar_w)   # light centre, ping-pong
    for x in range(x0, x0 + bar_w):
        dist = abs(x - cx)
        inten = int(255 * (1.0 - dist / glow)) if dist < glow else 18   # dim-red idle bar
        if inten < 18:
            inten = 18
        d.line([(x, y0), (x, y0 + bh)], fill=(inten, 0, 0))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="output C header path")
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--square", type=int, default=40)
    ap.add_argument("--quality", type=int, default=75)
    ap.add_argument("--dump-dir", default=None, help="also write .jpg files here")
    ap.add_argument("--solid", default=None,
                    help="R,G,B -> emit solid-colour frames (decode-vs-delivery test)")
    ap.add_argument("--image", default=None,
                    help="use this image file (resized to 320x240) as the frame(s)")
    ap.add_argument("--animate", action="store_true",
                    help="with --image, overlay a bouncing marker across --frames frames (live motion)")
    args = ap.parse_args()
    solid = tuple(int(x) for x in args.solid.split(",")) if args.solid else None

    img_src = None
    if args.image:
        img_src = Image.open(args.image).convert("RGB").resize((W, H), Image.LANCZOS)
        if args.animate:
            if args.frames < 2:
                args.frames = 24          # need multiple frames for motion
        else:
            args.frames = 1               # a static preview needs just one frame

    if args.dump_dir:
        os.makedirs(args.dump_dir, exist_ok=True)

    blobs = []
    for p in range(args.frames):
        if img_src is not None:
            img = animate_frame(img_src, p, args.frames) if args.animate else img_src
        elif solid:
            img = Image.new("RGB", (W, H), solid)
        else:
            img = make_frame(p, args.frames, args.square)
        import io
        buf = io.BytesIO()
        # baseline (progressive=False), 4:2:2 subsampling to match the OV519's
        # YH2V1 scan; complete tables so it decodes standalone.
        img.save(buf, format="JPEG", quality=args.quality,
                 progressive=False, subsampling="4:2:2", optimize=False)
        data = buf.getvalue()
        assert data[:4] == b"\xff\xd8\xff\xe0", "expected JFIF baseline SOI/APP0"
        blobs.append(data)
        if args.dump_dir:
            with open(os.path.join(args.dump_dir, f"cb_{p:02d}.jpg"), "wb") as f:
                f.write(data)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="\n") as f:
        f.write("// Generated by tools/gen_checkerboard_jpegs.py -- do not edit.\n")
        f.write("// Scrolling-checkerboard baseline-MJPEG frames for the Falcon\n")
        f.write("// OV519/OV530 (Xbox Video Camera) emulator.\n")
        f.write("#pragma once\n#include <stddef.h>\n\n")
        f.write(f"#define CHECKERBOARD_W {W}\n")
        f.write(f"#define CHECKERBOARD_H {H}\n")
        f.write(f"#define CHECKERBOARD_FRAME_COUNT {len(blobs)}\n\n")
        for i, data in enumerate(blobs):
            f.write(f"static const unsigned char cb_frame_{i}[] = {{\n")
            for j in range(0, len(data), 16):
                row = ", ".join(f"0x{b:02x}" for b in data[j:j + 16])
                f.write(f"    {row},\n")
            f.write("};\n")
        f.write("\nstatic const struct { const unsigned char *data; unsigned int len; } "
                "cb_frames[CHECKERBOARD_FRAME_COUNT] = {\n")
        for i in range(len(blobs)):
            f.write(f"    {{ cb_frame_{i}, (unsigned int)sizeof(cb_frame_{i}) }},\n")
        f.write("};\n")

    sizes = [len(b) for b in blobs]
    print(f"wrote {args.out}: {len(blobs)} frames, "
          f"sizes {min(sizes)}-{max(sizes)} B (avg {sum(sizes)//len(sizes)} B), "
          f"total {sum(sizes)} B")


if __name__ == "__main__":
    main()
