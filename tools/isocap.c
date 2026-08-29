// Falcon — isochronous ground-truth capture.
//
// Reads the camera's iso IN endpoint straight off the wire via libusb and
// reports, per packet, exactly how many bytes the HOST actually received.
// This answers the question no amount of Xbox-memory inspection could: does
// our device really put the MJPEG frame bytes on the bus?
//
// Build (in WSL): gcc -O2 -o isocap isocap.c $(pkg-config --libs --cflags libusb-1.0)
// Run:            ./isocap [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

#define VID 0x045e
#define PID 0x028c
#define EP  0x81
#define ALT 3
#define PKTS 8
#define PKTSZ 768
#define NXFER 4

static volatile int running = 1;
static unsigned long tot_pkt, tot_nonzero, tot_bytes, tot_sof, tot_eof, tot_soi, tot_err;
static int shown;

static void dump(const unsigned char *p, int len, const char *tag) {
    printf("   %s len=%d: ", tag, len);
    for (int i = 0; i < len && i < 24; i++) printf("%02x ", p[i]);
    printf("\n");
}

static void LIBUSB_CALL cb(struct libusb_transfer *xfr) {
    for (int i = 0; i < xfr->num_iso_packets; i++) {
        struct libusb_iso_packet_descriptor *d = &xfr->iso_packet_desc[i];
        tot_pkt++;
        if (d->status != LIBUSB_TRANSFER_COMPLETED) { tot_err++; continue; }
        int n = d->actual_length;
        if (!n) continue;
        tot_nonzero++; tot_bytes += n;
        unsigned char *p = libusb_get_iso_packet_buffer_simple(xfr, i);
        int is_sof = (n >= 4 && p[0]==0xff && p[1]==0xff && p[2]==0xff && p[3]==0x50);
        int is_eof = (n >= 4 && p[0]==0xff && p[1]==0xff && p[2]==0xff && p[3]==0x51);
        if (is_sof) tot_sof++;
        if (is_eof) tot_eof++;
        for (int k = 0; k + 3 < n; k++)
            if (p[k]==0xff && p[k+1]==0xd8 && p[k+2]==0xff) { tot_soi++; break; }
        if (shown < 80) {
            printf("%4d:%02x%02x%02x%02x%s", n, p[0], p[1], p[2], p[3],
                   (shown % 6 == 5) ? "\n" : "  ");
            shown++;
        }
    }
    if (running) libusb_submit_transfer(xfr);
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx)) { puts("libusb_init failed"); return 1; }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { printf("device %04x:%04x NOT FOUND\n", VID, PID); return 2; }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0)) { puts("claim_interface failed"); return 3; }
    if (libusb_set_interface_alt_setting(h, 0, ALT)) { puts("set alt 3 FAILED"); return 4; }
    printf("device open, interface 0 alt %d, reading EP 0x%02x for %ds...\n", ALT, EP, secs);

    struct libusb_transfer *x[NXFER];
    unsigned char *bufs[NXFER];
    for (int i = 0; i < NXFER; i++) {
        bufs[i] = calloc(1, PKTS * PKTSZ);
        x[i] = libusb_alloc_transfer(PKTS);
        libusb_fill_iso_transfer(x[i], h, EP, bufs[i], PKTS * PKTSZ, PKTS, cb, NULL, 1000);
        libusb_set_iso_packet_lengths(x[i], PKTSZ);
        if (libusb_submit_transfer(x[i])) printf("submit %d failed\n", i);
    }
    time_t t0 = time(NULL);
    while (time(NULL) - t0 < secs) {
        struct timeval tv = {0, 100000};
        libusb_handle_events_timeout(ctx, &tv);
    }
    running = 0;
    for (int i = 0; i < NXFER; i++) libusb_cancel_transfer(x[i]);
    for (int i = 0; i < 20; i++) { struct timeval tv={0,50000}; libusb_handle_events_timeout(ctx, &tv); }

    printf("\n==== RESULT over %ds ====\n", secs);
    printf("packets polled : %lu\n", tot_pkt);
    printf("  with DATA    : %lu   <-- key number\n", tot_nonzero);
    printf("  empty        : %lu\n", tot_pkt - tot_nonzero - tot_err);
    printf("  errored      : %lu\n", tot_err);
    printf("total bytes    : %lu\n", tot_bytes);
    printf("SOF (ffffff50) : %lu\n", tot_sof);
    printf("EOF (ffffff51) : %lu\n", tot_eof);
    printf("JPEG SOI seen  : %lu\n", tot_soi);
    libusb_release_interface(h, 0); libusb_close(h); libusb_exit(ctx);
    return 0;
}
