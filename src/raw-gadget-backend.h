#ifndef RAW_GADGET_BACKEND_H
#define RAW_GADGET_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RawGadgetDevice {
    int fd;
    int ep_in;
    int ep_out;
    int ep_in_addr;
    int ep_out_addr;
    bool device_open;
    int target_type;
    bool eps_enabled;

    pthread_t ep_out_thread;
    bool ep_out_thread_spawned;
    pthread_t ep0_thread;
    bool ep0_thread_spawned;
    bool configured;

    // USB_RAW_IOCTL_EP_WRITE blocks until the host actually reads the
    // packet (same as EP0_READ/EP_READ/EVENT_FETCH, which is why those
    // already run on dedicated threads) -- raw_gadget_send_input_report()
    // used to call it directly from whatever thread called it, which in
    // practice is this project's single-threaded main poll loop (the same
    // one serving ds4-ctl's IPC socket). If nothing is draining the IN
    // endpoint yet, that write blocks forever and freezes the whole
    // daemon. A dedicated ep_in_loop thread now owns the actual write;
    // raw_gadget_send_input_report() only ever stages the latest report
    // into this single-slot mailbox (older, unsent reports are simply
    // superseded -- each report carries full state, not a delta, so
    // dropping a stale one is harmless) and returns immediately.
    pthread_t ep_in_thread;
    bool ep_in_thread_spawned;
    pthread_mutex_t report_mutex;
    pthread_cond_t report_cond;
    uint8_t pending_report[64];
    size_t pending_report_len;
    bool report_pending;
};

bool raw_gadget_init(struct RawGadgetDevice *dev, int target_type);
void raw_gadget_close(struct RawGadgetDevice *dev);
bool raw_gadget_send_input_report(struct RawGadgetDevice *dev, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // RAW_GADGET_BACKEND_H
