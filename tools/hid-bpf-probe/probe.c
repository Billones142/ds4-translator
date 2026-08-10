// SPDX-License-Identifier: GPL-2.0-only
//
// Phase 1 spike consumer: reads the ring buffer probe.bpf.c pins at
// /sys/fs/bpf/ds4_reports and dumps every mirrored DS4 report to
// stdout. Does not load or attach the BPF program itself -- run
// `udev-hid-bpf` against probe.bpf.o first (see README.md), then run
// this once the pinned map exists.

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_REPORT_LEN 78
#define PIN_PATH "/sys/fs/bpf/ds4_reports"

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static int on_report(void *ctx, void *data, size_t data_sz) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("[%ld.%03ld] %zu bytes:", (long)ts.tv_sec, ts.tv_nsec / 1000000, data_sz);
    const unsigned char *b = (const unsigned char *)data;
    size_t n = data_sz < MAX_REPORT_LEN ? data_sz : MAX_REPORT_LEN;
    for (size_t i = 0; i < n; ++i) {
        printf(" %02x", b[i]);
    }
    printf("\n");
    return 0;
}

int main(void) {
    int map_fd = bpf_obj_get(PIN_PATH);
    if (map_fd < 0) {
        fprintf(stderr, "probe: bpf_obj_get(%s) failed: %s\n"
                         "probe: is probe.bpf.o loaded via udev-hid-bpf yet? see README.md\n",
                PIN_PATH, strerror(errno));
        return 1;
    }

    struct ring_buffer *rb = ring_buffer__new(map_fd, on_report, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "probe: ring_buffer__new failed: %s\n", strerror(errno));
        return 1;
    }

    signal(SIGINT, on_sigint);
    fprintf(stderr, "probe: watching %s, ctrl-c to stop\n", PIN_PATH);

    while (!g_stop) {
        int err = ring_buffer__poll(rb, 200 /* ms */);
        if (err < 0 && err != -EINTR) {
            fprintf(stderr, "probe: ring_buffer__poll: %s\n", strerror(-err));
            break;
        }
    }

    ring_buffer__free(rb);
    return 0;
}
