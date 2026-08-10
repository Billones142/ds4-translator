// SPDX-License-Identifier: GPL-2.0-only
//
// Phase 1 spike consumer: reads the ring buffer probe.bpf.c's ds4_reports
// map, pinned by udev-hid-bpf's own loader (not by probe.bpf.c -- see its
// comment on the map definition) at
// /sys/fs/bpf/hid/<hid_id_with_underscores>/probe_bpf/ds4_reports, and
// dumps every mirrored DS4 report to stdout. Does not load or attach the
// BPF program itself -- run `udev-hid-bpf add` against probe.bpf.o first
// (see README.md), then run this with that pin path as argv[1].

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_REPORT_LEN 78

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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <pin-path-to-ds4_reports-map>\n"
                         "  e.g. /sys/fs/bpf/hid/0005_054C_05C4_002F/probe_bpf/ds4_reports\n"
                         "  (see README.md -- udev-hid-bpf's own loader picks this path)\n",
                argv[0]);
        return 1;
    }
    const char *pin_path = argv[1];

    int map_fd = bpf_obj_get(pin_path);
    if (map_fd < 0) {
        fprintf(stderr, "probe: bpf_obj_get(%s) failed: %s\n"
                         "probe: is probe.bpf.o loaded via udev-hid-bpf yet? see README.md\n",
                pin_path, strerror(errno));
        return 1;
    }

    struct ring_buffer *rb = ring_buffer__new(map_fd, on_report, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "probe: ring_buffer__new failed: %s\n", strerror(errno));
        return 1;
    }

    signal(SIGINT, on_sigint);
    fprintf(stderr, "probe: watching %s, ctrl-c to stop\n", pin_path);

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
