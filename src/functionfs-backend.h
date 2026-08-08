#ifndef FUNCTIONFS_BACKEND_H
#define FUNCTIONFS_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FunctionFSDevice {
    int ep0_fd;
    int ep_in_fd;
    int ep_out_fd;
    bool device_open;
    bool configured; // true between FUNCTIONFS_ENABLE and FUNCTIONFS_DISABLE
    int target_type;

    // Unlike hidg (kernel's f_hid handles enumeration/SET_CONFIGURATION
    // in-kernel), FunctionFS delivers control events to userspace on ep0
    // the same way raw-gadget does -- but only the subset the kernel's
    // gadget/UDC core doesn't already answer itself (GET_DESCRIPTOR for
    // DEVICE/CONFIG/STRING, SET_CONFIGURATION, SET_INTERFACE all happen
    // below FunctionFS entirely). What's left to handle here is just the
    // HID-specific class requests and the HID report descriptor fetch.
    pthread_t ep0_thread;
    bool ep0_thread_spawned;

    pthread_t out_thread;
    bool out_thread_spawned;

    // write() on ep1 blocks until the host reads the packet, same reasoning
    // as the other two backends' report_mutex/report_cond fields -- a
    // dedicated in_thread owns the actual write(); functionfs_send_input_report()
    // only ever stages the latest report into this single-slot mailbox.
    pthread_t in_thread;
    bool in_thread_spawned;
    pthread_mutex_t report_mutex;
    pthread_cond_t report_cond;
    uint8_t pending_report[64];
    size_t pending_report_len;
    bool report_pending;
};

bool functionfs_init(struct FunctionFSDevice *dev, int target_type);
void functionfs_close(struct FunctionFSDevice *dev);
bool functionfs_send_input_report(struct FunctionFSDevice *dev, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // FUNCTIONFS_BACKEND_H
