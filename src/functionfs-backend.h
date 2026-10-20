#ifndef FUNCTIONFS_BACKEND_H
#define FUNCTIONFS_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Values for FunctionFSDevice::target_type / functionfs_init()'s target_type.
enum {
    FFS_TARGET_DS4 = 1,
    FFS_TARGET_DUALSENSE = 2,
    // Vendor-class (non-HID) gadget bound by the kernel's xpad driver.
    FFS_TARGET_XBOX360 = 3,
    // Xbox One family: same vendor-class USB device bound by xpad, speaking
    // GIP instead of the 360's fixed packet format.
    FFS_TARGET_XBOXONE = 4,
    FFS_TARGET_XBOXSERIES = 5
};

// Queued, never-dropped packets (e.g. a GIP Guide press/release) wait here,
// unlike the single-slot input report mailbox where the newest report wins.
#define FFS_EVENT_QUEUE_SIZE 8
#define FFS_PACKET_MAX 64

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
    uint8_t pending_report[FFS_PACKET_MAX];
    size_t pending_report_len;
    bool report_pending;

    uint8_t event_queue[FFS_EVENT_QUEUE_SIZE][FFS_PACKET_MAX];
    size_t event_queue_len[FFS_EVENT_QUEUE_SIZE];
    size_t event_head;  // oldest queued packet
    size_t event_count;
};

// Sets the MAC the emulated device advertises: the serialnumber string, plus
// the byte-reversed form the DS4/DualSense pairing feature reports carry. Must
// be called before functionfs_init(); left unset, the built-in default MAC is
// used, exactly as before this existed.
void functionfs_set_mac(const uint8_t mac_le[6], const char *mac_str);

// product_name: custom USB product string override, or NULL/empty to use the
// built-in per-type default ("Wireless Controller" / "DualSense Wireless
// Controller"). Ignored for the Xbox targets: xpad names the device itself.
bool functionfs_init(struct FunctionFSDevice *dev, int target_type, const char *product_name);
void functionfs_close(struct FunctionFSDevice *dev);
bool functionfs_send_input_report(struct FunctionFSDevice *dev, const uint8_t *data, size_t size);
// Queues a packet that must reach the host in order and not be superseded by
// a newer report. It goes out before the pending input report. If the queue is
// full the oldest packet is dropped.
bool functionfs_send_event_packet(struct FunctionFSDevice *dev, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // FUNCTIONFS_BACKEND_H
