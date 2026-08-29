#!/usr/bin/env python3
"""
Falcon -- re-apply the ESP32-S3 dwc2 EP0-SETUP-DMA crash fix to esp_tinyusb.

esp_tinyusb / tinyusb are ESP-IDF *managed components*: the component manager
re-downloads them pristine (they are gitignored), so this one-line source fix
cannot be committed with the rest of the tree. This script re-applies it.

It is idempotent (a no-op if already patched) and version-tolerant (it targets
just the single EP0 SETUP-packet-pointer line, matched by content, not by line
number), and it is invoked automatically from main/CMakeLists.txt during the
build. You can also run it by hand after `idf.py reconfigure`:

    python patches/apply_tinyusb_patch.py            # from the project root
    python patches/apply_tinyusb_patch.py <proj_dir>

Background: on the ESP32-S3, reading DOEPDMA0 to locate the last SETUP packet can
return a garbage pointer when a SETUP overlaps heavy iso-IN traffic; the ensuing
memcpy in dcd_event_setup_received faults (LoadProhibited). The SETUP packet is
always DMA'd into _dcd_usbbuf.setup_buffer, so we clamp the pointer to that
buffer. Without this fix, DMA mode crashes ~2-3s into streaming on the Xbox.
"""
import os
import re
import sys

MARKER = "FALCON FIX"

# The pristine line assigns setup_packet from epout0->doepdma on a single line.
# Match it by content (tolerant of the exact cast style across tinyusb variants),
# and NOT the already-patched multi-line form (its setup_packet line has no
# epout0->doepdma before the ';').
PRISTINE_RE = re.compile(
    r'^[ \t]*tusb_control_request_t\s*\*\s*setup_packet\s*=[^\n;]*epout0->doepdma[^\n;]*;[ \t]*\r?\n',
    re.MULTILINE,
)

REPLACEMENT = (
    "    // FALCON FIX: On ESP32-S3, DOEPDMA0 readback can return a garbage pointer when a SETUP overlaps heavy\n"
    "    // iso-IN traffic, which then crashes the memcpy in dcd_event_setup_received (LoadProhibited). The SETUP\n"
    "    // packet is always DMA'd into _dcd_usbbuf.setup_buffer, so clamp: use doepdma-8 only when it lands inside\n"
    "    // that buffer (handles legit back-to-back setups); otherwise fall back to the known buffer base.\n"
    "    const uintptr_t sb_base = (uintptr_t) _dcd_usbbuf.setup_buffer;\n"
    "    const uintptr_t sb_end  = sb_base + sizeof(_dcd_usbbuf.setup_buffer);\n"
    "    const uintptr_t dma_ptr = (uintptr_t) epout0->doepdma;\n"
    "    tusb_control_request_t *setup_packet =\n"
    "        (dma_ptr > sb_base && dma_ptr <= sb_end)\n"
    "            ? (tusb_control_request_t *) (dma_ptr - sizeof(tusb_control_request_t))\n"
    "            : (tusb_control_request_t *) sb_base;\n"
)

REL = os.path.join("src", "portable", "synopsys", "dwc2", "dcd_dwc2.c")


def find_target(proj):
    mc = os.path.join(proj, "managed_components")
    candidates = [
        os.path.join(mc, "espressif__tinyusb", REL),
        os.path.join(proj, "components", "tinyusb", REL),
    ]
    if os.path.isdir(mc):
        for name in os.listdir(mc):
            if "tinyusb" in name:
                candidates.append(os.path.join(mc, name, REL))
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def main():
    proj = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = find_target(proj)
    if path is None:
        # Deps not fetched yet -- nothing to patch. It will apply on a later run.
        print("dcd_dwc2.c not found (tinyusb not fetched yet); skipping")
        return 0
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        src = f.read()
    if MARKER in src:
        print("already patched")
        return 0
    new, n = PRISTINE_RE.subn(REPLACEMENT, src, count=1)
    if n != 1:
        sys.stderr.write(
            "ERROR: could not find the EP0 setup_packet line to patch in %s\n"
            "       (tinyusb version drift?). Apply the fix by hand -- see patches/README.md.\n" % path
        )
        return 1
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(new)
    print("patched %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
