#ifndef HIDG_BACKEND_H
#define HIDG_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct HidgDevice {
    int fd;
    bool device_open;
    bool configured;
    int target_type;

    // Kernel's f_hid function handles USB control transfers (enumeration,
    // SET_CONFIGURATION, GET_REPORT/SET_REPORT defaults) entirely in-kernel
    // -- unlike raw-gadget, there's no ep0 event loop to run in userspace.
    // OUT reports (rumble/LED, host->device) arrive via read() on the same
    // /dev/hidgN node; a dedicated thread owns that blocking read().
    pthread_t out_thread;
    bool out_thread_spawned;

    // write() on /dev/hidgN blocks until the host reads the packet, same
    // reasoning as raw-gadget-backend.h's report_mutex/report_cond fields --
    // a dedicated in_thread owns the actual write(); hidg_send_input_report()
    // only ever stages the latest report into this single-slot mailbox.
    pthread_t in_thread;
    bool in_thread_spawned;
    pthread_mutex_t report_mutex;
    pthread_cond_t report_cond;
    uint8_t pending_report[64];
    size_t pending_report_len;
    bool report_pending;

    // f_hid's control-transfer GET_REPORT (feature reports -- DS4/DualSense
    // pairing info, calibration, firmware version) has no data path of its
    // own: the kernel blocks the pending control transfer until userspace
    // answers via the GADGET_HID_READ_GET_REPORT_ID / GADGET_HID_WRITE_GET_REPORT
    // ioctls on the same fd. Own thread, since GADGET_HID_READ_GET_REPORT_ID
    // blocks.
    pthread_t get_report_thread;
    bool get_report_thread_spawned;
};

bool hidg_init(struct HidgDevice *dev, int target_type);
void hidg_close(struct HidgDevice *dev);
bool hidg_send_input_report(struct HidgDevice *dev, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // HIDG_BACKEND_H
