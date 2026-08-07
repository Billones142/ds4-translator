#include "raw-gadget-backend.h"
#include "descriptors.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <linux/usb/ch9.h>
#include <signal.h>

/* Debug logging — compile with -DDS4_DEBUG to enable */
#ifdef DS4_DEBUG
#  define DBG(fmt, ...) fprintf(stderr, "[ds4-debug] " fmt "\n", ##__VA_ARGS__)
#else
#  define DBG(fmt, ...) ((void)0)
#endif

static void dummy_signal_handler(int sig) {
    (void)sig; // Just to interrupt blocking ioctl
}

// Raw Gadget definitions
#define UDC_NAME_LENGTH_MAX 128

struct usb_raw_init {
    uint8_t driver_name[UDC_NAME_LENGTH_MAX];
    uint8_t device_name[UDC_NAME_LENGTH_MAX];
    uint8_t speed;
};

enum usb_raw_event_type {
    USB_RAW_EVENT_INVALID = 0,
    USB_RAW_EVENT_CONNECT = 1,
    USB_RAW_EVENT_CONTROL = 2,
    USB_RAW_EVENT_SUSPEND = 3,
    USB_RAW_EVENT_RESUME = 4,
    USB_RAW_EVENT_RESET = 5,
    USB_RAW_EVENT_DISCONNECT = 6,
};

struct usb_raw_event {
    uint32_t type;
    uint32_t length;
    uint8_t data[];
};

struct usb_raw_ep_io {
    uint16_t ep;
    uint16_t flags;
    uint32_t length;
    uint8_t data[];
};

#define USB_RAW_EPS_NUM_MAX 30
#define USB_RAW_EP_NAME_MAX 16
#define USB_RAW_EP_ADDR_ANY 0xff

struct usb_raw_ep_caps {
    uint32_t type_control : 1;
    uint32_t type_iso : 1;
    uint32_t type_bulk : 1;
    uint32_t type_int : 1;
    uint32_t dir_in : 1;
    uint32_t dir_out : 1;
};

struct usb_raw_ep_limits {
    uint16_t maxpacket_limit;
    uint16_t max_streams;
    uint32_t reserved;
};

struct usb_raw_ep_info {
    uint8_t name[USB_RAW_EP_NAME_MAX];
    uint32_t addr;
    struct usb_raw_ep_caps caps;
    struct usb_raw_ep_limits limits;
};

struct usb_raw_eps_info {
    struct usb_raw_ep_info eps[USB_RAW_EPS_NUM_MAX];
};

#define USB_RAW_IOCTL_INIT _IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN _IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH _IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE _IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ _IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE _IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE _IOW('U', 6, uint32_t)
#define USB_RAW_IOCTL_EP_WRITE _IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ _IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE _IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW _IOW('U', 10, uint32_t)
#define USB_RAW_IOCTL_EPS_INFO _IOR('U', 11, struct usb_raw_eps_info)
#define USB_RAW_IOCTL_EP0_STALL _IO('U', 12)
#define USB_RAW_IOCTL_EP_SET_HALT _IOW('U', 13, uint32_t)

struct usb_raw_control_event {
    struct usb_raw_event inner;
    struct usb_ctrlrequest ctrl;
};

#define EP0_MAX_DATA 2048
struct usb_raw_control_io {
    struct usb_raw_ep_io inner;
    uint8_t data[EP0_MAX_DATA];
};

// Global raw-gadget structures
static struct usb_device_descriptor device_desc_ds4 = {
    .bLength = 18,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x054c,
    .idProduct = 0x05c4,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1
};

static struct usb_device_descriptor device_desc_dualsense = {
    .bLength = 18,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x054c,
    .idProduct = 0x0ce6,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1
};

static int encode_string_desc(const char* str, uint8_t* buf, int max_len) {
    int len = strlen(str);
    int size = 2 + len * 2;
    if (size > max_len) size = max_len;
    buf[0] = size;
    buf[1] = USB_DT_STRING;
    for (int i = 0; i < len && (2 + i * 2 + 1) < size; ++i) {
        buf[2 + i * 2] = str[i];
        buf[2 + i * 2 + 1] = 0;
    }
    return size;
}

static void process_eps_info(struct RawGadgetDevice *dev) {
    struct usb_raw_eps_info info;
    memset(&info, 0, sizeof(info));
    int num = ioctl(dev->fd, USB_RAW_IOCTL_EPS_INFO, &info);
    if (num < 0) {
        perror("ioctl(USB_RAW_IOCTL_EPS_INFO)");
        return;
    }

    // Find first IN endpoint and first OUT endpoint
    for (int i = 0; i < num; ++i) {
        if (info.eps[i].caps.type_int) {
            if (info.eps[i].caps.dir_in && dev->ep_in_addr < 0) {
                dev->ep_in_addr = info.eps[i].addr == USB_RAW_EP_ADDR_ANY ? 1 : info.eps[i].addr;
            }
            if (info.eps[i].caps.dir_out && dev->ep_out_addr < 0) {
                dev->ep_out_addr = info.eps[i].addr == USB_RAW_EP_ADDR_ANY ? 1 : info.eps[i].addr;
            }
        }
    }
    if (dev->ep_in_addr < 0) dev->ep_in_addr = 1;
    if (dev->ep_out_addr < 0) dev->ep_out_addr = 1;
}

// Global output report buffers and state tracking (imported from main.cpp via extern)
extern uint8_t cur_motor_left, cur_motor_right, cur_r, cur_g, cur_b;
extern int phy_fd;
extern bool is_bluetooth;
extern void send_physical_output_report(int fd, bool bluetooth, uint8_t motor_left, uint8_t motor_right, uint8_t r, uint8_t g, uint8_t b);

static void* ep0_loop(void *arg);   /* forward declaration */
static void* ep_in_loop(void *arg); /* forward declaration */

static void* ep_out_loop(void *arg) {
    struct RawGadgetDevice *dev = (struct RawGadgetDevice *)arg;
    int fd = dev->fd;
    
    // Out packet loop (reads LED/rumble events)
    struct usb_raw_ep_io *io = (struct usb_raw_ep_io *)malloc(sizeof(struct usb_raw_ep_io) + 64);
    
    while (dev->device_open) {
        if (!dev->configured || !dev->eps_enabled || dev->ep_out < 0) {
            usleep(10000); // 10ms
            continue;
        }
        
        io->ep = dev->ep_out;
        io->flags = 0;
        io->length = 64;
        
        int rv = ioctl(fd, USB_RAW_IOCTL_EP_READ, io);
        if (rv < 0) {
            if (errno == EINTR) {
                if (!dev->device_open) break;
                continue;
            }
            usleep(10000);
            continue;
        }
        if (rv > 0) {
            uint8_t *data = io->data;
            uint8_t motor_left = cur_motor_left;
            uint8_t motor_right = cur_motor_right;
            uint8_t r = cur_r, g = cur_g, b = cur_b;
            bool update = false;

            if (dev->target_type == 1) { // TYPE_DS4
                if (data[0] == 0x05 && rv >= 9) {
                    uint8_t flags = data[1];
                    if (flags & 0x01) {
                        motor_right = data[4];
                        motor_left = data[5];
                        update = true;
                    }
                    if (flags & 0x02) {
                        r = data[6];
                        g = data[7];
                        b = data[8];
                        update = true;
                    }
                }
            } else { // DualSense
                if (data[0] == 0x02 && rv >= 48) {
                    uint8_t vf0 = data[1];
                    uint8_t vf1 = data[2];
                    if (vf0 & 0x01) {
                        motor_right = data[3];
                        motor_left = data[4];
                        if ((vf0 & 0x02) && motor_right > 0 && motor_left > 0) {
                            motor_left = 0;
                        }
                        update = true;
                    }
                    if (vf1 & 0x04) {
                        r = data[45];
                        g = data[46];
                        b = data[47];
                        update = true;
                    }
                }
            }

            if (update && phy_fd >= 0) {
                if (motor_left != cur_motor_left || motor_right != cur_motor_right || r != cur_r || g != cur_g || b != cur_b) {
                    cur_motor_left = motor_left;
                    cur_motor_right = motor_right;
                    cur_r = r;
                    cur_g = g;
                    cur_b = b;
                    send_physical_output_report(phy_fd, is_bluetooth, cur_motor_left, cur_motor_right, cur_r, cur_g, cur_b);
                }
            }
        }
    }
    free(io);
    return NULL;
}

// Acks the status stage of a no-data-stage (or already-read data-OUT)
// control request. A control transfer's status stage is always the
// opposite direction of its data stage, or IN when there's no data stage
// at all -- never simply "whatever bmRequestType's direction bit says",
// which is what the generic reply dispatch further down actually checks
// (correct only for requests that themselves carry an IN/OUT data stage).
// So the correct ack for e.g. SET_CONFIGURATION/SET_INTERFACE/SET_IDLE
// (no data stage) and SET_REPORT (data-OUT stage, already read) is always
// a zero-length EP0_WRITE.
//
// Call this as early as possible after receiving the event -- confirmed
// by disassembling this system's actual raw_gadget.ko (no kernel source
// package available, so this is ground truth rather than a guess):
// raw_process_ep0_io() requires the driver's own internal
// ep0_in_pending/ep0_out_pending flags (and a dev->state precondition) to
// still be valid when this is called, and delaying it behind slower
// side-effect ioctls (EP_ENABLE, CONFIGURE) was observed to invalidate
// that pending state before the ack got a chance to run -- manifesting
// as EBUSY (wrong/stale pending-direction flag) or EINVAL (dev->state
// moved on) depending on timing, and, before this function existed at
// all, as the gadget staying stuck unconfigured from the host's
// perspective (bConfigurationValue/interface sysfs entries never
// appearing) even though USB_RAW_IOCTL_CONFIGURE itself returned success.
static void ack_zero_length_status(struct RawGadgetDevice *dev, const char *what) {
    struct usb_raw_control_io status_io;
    memset(&status_io, 0, sizeof(status_io));
    status_io.inner.ep = 0;
    status_io.inner.length = 0;
    if (ioctl(dev->fd, USB_RAW_IOCTL_EP0_WRITE, &status_io) < 0) {
        fprintf(stderr, "ioctl(USB_RAW_IOCTL_EP0_WRITE) [%s status ack]: %s\n", what, strerror(errno));
    }
}

static bool handle_control_request(struct RawGadgetDevice *dev, void *event_ptr) {
    struct usb_raw_control_event *event = (struct usb_raw_control_event *)event_ptr;
    struct usb_ctrlrequest *ctrl = &event->ctrl;
    DBG("EP0 req: reqType=0x%02x, req=0x%02x, val=0x%04x, idx=0x%04x, len=%d",
        ctrl->bRequestType, ctrl->bRequest, ctrl->wValue, ctrl->wIndex, ctrl->wLength);
    
    struct usb_raw_control_io io;
    memset(&io, 0, sizeof(io));
    io.inner.ep = 0;
    io.inner.flags = 0;
    io.inner.length = 0;
    
    bool reply = false;

    if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
        switch (ctrl->bRequest) {
            case USB_REQ_GET_DESCRIPTOR: {
                uint8_t type = ctrl->wValue >> 8;
                uint8_t index = ctrl->wValue & 0xFF;
                
                if (type == USB_DT_DEVICE) {
                    if (dev->target_type == 1) {
                        memcpy(&io.data[0], &device_desc_ds4, 18);
                    } else {
                        memcpy(&io.data[0], &device_desc_dualsense, 18);
                    }
                    io.inner.length = 18;
                    reply = true;
                } else if (type == USB_DT_DEVICE_QUALIFIER) {
                    io.data[0] = 10;
                    io.data[1] = USB_DT_DEVICE_QUALIFIER;
                    io.data[2] = 0x00; io.data[3] = 0x02;
                    io.data[4] = 0; io.data[5] = 0; io.data[6] = 0;
                    io.data[7] = 64;
                    io.data[8] = 1;
                    io.data[9] = 0;
                    io.inner.length = 10;
                    reply = true;
                } else if (type == USB_DT_CONFIG || type == USB_DT_OTHER_SPEED_CONFIG) {
                    uint8_t *ptr = io.data;
                    
                    // Config
                    ptr[0] = 9; ptr[1] = type;
                    uint16_t tot_len = 9 + 9 + 9 + 7 + 7;
                    ptr[2] = tot_len & 0xFF; ptr[3] = tot_len >> 8;
                    ptr[4] = 1; ptr[5] = 1; ptr[6] = 0;
                    ptr[7] = 0xC0; ptr[8] = 250;
                    ptr += 9;
                    
                    // Interface
                    ptr[0] = 9; ptr[1] = 4; ptr[2] = 0; ptr[3] = 0;
                    ptr[4] = 2; // 2 endpoints
                    ptr[5] = 3; // Class: HID
                    ptr[6] = 0; ptr[7] = 0; ptr[8] = 0;
                    ptr += 9;
                    
                    // HID
                    ptr[0] = 9; ptr[1] = 0x21;
                    ptr[2] = 0x11; ptr[3] = 0x01; // HID v1.11
                    ptr[4] = 0; ptr[5] = 1; ptr[6] = 0x22; // Report type
                    uint16_t rdesc_len = (dev->target_type == 1) ? sizeof(ds4_usb_rdesc) : sizeof(dualsense_usb_rdesc);
                    ptr[7] = rdesc_len & 0xFF; ptr[8] = rdesc_len >> 8;
                    ptr += 9;
                    
                    // Endpoint IN
                    ptr[0] = 7; ptr[1] = 5; ptr[2] = 0x80 | dev->ep_in_addr; // EP IN
                    ptr[3] = 3; // Interrupt
                    ptr[4] = 64; ptr[5] = 0; // Packets size 64
                    ptr[6] = 4; // Interval 4ms
                    ptr += 7;
                    
                    // Endpoint OUT
                    ptr[0] = 7; ptr[1] = 5; ptr[2] = dev->ep_out_addr; // EP OUT
                    ptr[3] = 3; // Interrupt
                    ptr[4] = 64; ptr[5] = 0; // Packets size 64
                    ptr[6] = 4; // Interval 4ms
                    ptr += 7;
                    
                    io.inner.length = tot_len;
                    reply = true;
                } else if (type == USB_DT_STRING) {
                    if (index == 0) {
                        io.data[0] = 4; io.data[1] = 3;
                        io.data[2] = 0x09; io.data[3] = 0x04;
                        io.inner.length = 4;
                        reply = true;
                    } else if (index == 1) {
                        io.inner.length = encode_string_desc(dev->target_type == 1 ? "Sony Computer Entertainment" : "Sony Interactive Entertainment", io.data, EP0_MAX_DATA);
                        reply = true;
                    } else if (index == 2) {
                        io.inner.length = encode_string_desc("Wireless Controller", io.data, EP0_MAX_DATA);
                        reply = true;
                    } else if (index == 3) {
                        io.inner.length = encode_string_desc("74:e7:d6:3a:47:e8", io.data, EP0_MAX_DATA);
                        reply = true;
                    } else if (index == 4) {
                        io.inner.length = encode_string_desc("Default Configuration", io.data, EP0_MAX_DATA);
                        reply = true;
                    } else if (index == 5) {
                        io.inner.length = encode_string_desc("Wireless Controller", io.data, EP0_MAX_DATA);
                        reply = true;
                    }
                } else if (type == 0x21) { // HID
                    io.data[0] = 9; io.data[1] = 0x21;
                    io.data[2] = 0x11; io.data[3] = 0x01;
                    io.data[4] = 0; io.data[5] = 1; io.data[6] = 0x22;
                    uint16_t rdesc_len = (dev->target_type == 1) ? sizeof(ds4_usb_rdesc) : sizeof(dualsense_usb_rdesc);
                    io.data[7] = rdesc_len & 0xFF; io.data[8] = rdesc_len >> 8;
                    io.inner.length = 9;
                    reply = true;
                } else if (type == 0x22) { // Report
                    if (dev->target_type == 1) {
                        memcpy(io.data, ds4_usb_rdesc, sizeof(ds4_usb_rdesc));
                        io.inner.length = sizeof(ds4_usb_rdesc);
                    } else {
                        memcpy(io.data, dualsense_usb_rdesc, sizeof(dualsense_usb_rdesc));
                        io.inner.length = sizeof(dualsense_usb_rdesc);
                    }
                    reply = true;
                }
                break;
            }
            case USB_REQ_SET_CONFIGURATION: {
                // Acked FIRST, before any of the slower EP_ENABLE/CONFIGURE
                // side-effect calls below -- confirmed by disassembling the
                // actual raw_gadget.ko on this system (no kernel source
                // package available, so this is from ground truth rather
                // than guessing): raw_process_ep0_io() requires the
                // driver's own dev->ep0_in_pending/ep0_out_pending flags
                // (and a dev->state precondition) to still be valid at ack
                // time, or it fails outright -- EBUSY for a stale/wrong
                // pending-direction flag, EINVAL if dev->state moved on.
                // Both were observed live across earlier attempts at this
                // fix, with the common thread being that EP_ENABLE/
                // CONFIGURE ran *before* the ack and evidently gave the
                // driver enough time/opportunity to invalidate that pending
                // state before we got to it. Acking immediately avoids the
                // race entirely.
                ack_zero_length_status(dev, "SET_CONFIGURATION");

                struct usb_endpoint_descriptor ep_in_desc = {
                    .bLength = 7,
                    .bDescriptorType = 5,
                    .bEndpointAddress = (uint8_t)(0x80 | dev->ep_in_addr),
                    .bmAttributes = 3,
                    .wMaxPacketSize = 64,
                    .bInterval = 4
                };
                int ep_in_id = ioctl(dev->fd, USB_RAW_IOCTL_EP_ENABLE, &ep_in_desc);
                if (ep_in_id < 0) {
                    perror("ioctl(USB_RAW_IOCTL_EP_ENABLE IN)");
                } else {
                    dev->ep_in = ep_in_id;
                }

                struct usb_endpoint_descriptor ep_out_desc = {
                    .bLength = 7,
                    .bDescriptorType = 5,
                    .bEndpointAddress = (uint8_t)dev->ep_out_addr,
                    .bmAttributes = 3,
                    .wMaxPacketSize = 64,
                    .bInterval = 4
                };
                int ep_out_id = ioctl(dev->fd, USB_RAW_IOCTL_EP_ENABLE, &ep_out_desc);
                if (ep_out_id < 0) {
                    perror("ioctl(USB_RAW_IOCTL_EP_ENABLE OUT)");
                } else {
                    dev->ep_out = ep_out_id;
                }

                ioctl(dev->fd, USB_RAW_IOCTL_CONFIGURE, 0);

                dev->device_open = true;
                dev->eps_enabled = true;
                return true;
            }
            case USB_REQ_GET_INTERFACE: {
                io.data[0] = 0;
                io.inner.length = 1;
                reply = true;
                break;
            }
            case USB_REQ_SET_INTERFACE: {
                ack_zero_length_status(dev, "SET_INTERFACE");
                return true;
            }
        }
    } else if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS) {
        switch (ctrl->bRequest) {
            case 0x01: { // GET_REPORT
                uint8_t rnum = ctrl->wValue & 0xFF;
                uint8_t rtype = ctrl->wValue >> 8;
                if (rtype == 3) {
                    if (dev->target_type == 1) {
                        if (rnum == 0x02) {
                            io.data[0] = 0x02;
                            io.data[7] = 0x00; io.data[8] = 0x04;
                            io.data[9] = 0x00; io.data[10] = 0xFC;
                            io.data[11] = 0x00; io.data[12] = 0x04;
                            io.data[13] = 0x00; io.data[14] = 0xFC;
                            io.data[15] = 0x00; io.data[16] = 0x04;
                            io.data[17] = 0x00; io.data[18] = 0xFC;
                            io.data[19] = 0x00; io.data[20] = 0x04;
                            io.data[21] = 0x00; io.data[22] = 0x04;
                            io.data[23] = 0x00; io.data[24] = 0x20;
                            io.data[25] = 0x00; io.data[26] = 0xE0;
                            io.data[27] = 0x00; io.data[28] = 0x20;
                            io.data[29] = 0x00; io.data[30] = 0xE0;
                            io.data[31] = 0x00; io.data[32] = 0x20;
                            io.data[33] = 0x00; io.data[34] = 0xE0;
                            io.inner.length = 37;
                            reply = true;
                        } else if (rnum == 0xa3) {
                            io.data[0] = 0xa3;
                            io.data[35] = 0x38; io.data[36] = 0x54;
                            io.data[41] = 0x33; io.data[42] = 0x20;
                            io.inner.length = 49;
                            reply = true;
                        } else if (rnum == 0x12) {
                            io.data[0] = 0x12;
                            io.data[1] = 0xe8; io.data[2] = 0x47; io.data[3] = 0x3a;
                            io.data[4] = 0xd6; io.data[5] = 0xe7; io.data[6] = 0x74;
                            io.inner.length = 16;
                            reply = true;
                        }
                    } else {
                        if (rnum == 0x05) {
                            uint8_t cal_data[41] = {
                                0x05,
                                0xff, 0xfc, 0xff, 0xfe, 0xff, 0x83, 0x22, 0x78,
                                0xdd, 0x92, 0x22, 0x5f, 0xdd, 0x95, 0x22, 0x6d,
                                0xdd, 0x1c, 0x02, 0x1c, 0x02, 0xf2, 0x1f, 0xed,
                                0xdf, 0xe3, 0x20, 0xda, 0xe0, 0xee, 0x1f, 0xdf,
                                0xdf, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                            };
                            memcpy(io.data, cal_data, 41);
                            io.inner.length = 41;
                            reply = true;
                        } else if (rnum == 0x20) {
                            uint8_t fw_data[64] = {
                                0x20,
                                0x4a, 0x75, 0x6e, 0x20, 0x31, 0x39, 0x20, 0x32, 0x30, 0x32, 0x33,
                                0x31, 0x34, 0x3a, 0x34, 0x37, 0x3a, 0x33, 0x34,
                                0x03, 0x00, 0x44, 0x00, 0x08, 0x02, 0x00, 0x01,
                                0x36, 0x00, 0x00, 0x01, 0xc1, 0xc8, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x54, 0x01, 0x00, 0x00, 0x14, 0x00,
                                0x00, 0x00, 0x0b, 0x00, 0x01, 0x00, 0x06, 0x00,
                                0x00, 0x00, 0x00, 0x00
                            };
                            memcpy(io.data, fw_data, 64);
                            io.inner.length = 64;
                            reply = true;
                        } else if (rnum == 0x09) {
                            uint8_t pairing_data[20] = {
                                0x09,
                                0xe8, 0x47, 0x3a, 0xd6, 0xe7, 0x74,
                                0x08, 0x25, 0x00, 0x1e, 0x00, 0xee, 0x74, 0xd0, 0xbc,
                                0x00, 0x00, 0x00, 0x00
                            };
                            memcpy(io.data, pairing_data, 20);
                            io.inner.length = 20;
                            reply = true;
                        }
                    }
                }
                break;
            }
            case 0x09: { // SET_REPORT
                // ep0_read_io used to be a bare `struct usb_raw_ep_io` (8
                // bytes: ep+flags+length, no trailing buffer) with the
                // kernel told to read ctrl->wLength bytes into its
                // zero-length flexible-array `data[]` member -- for any
                // wLength > 0 (every real SET_REPORT, e.g. 9 bytes for a
                // DS4 rumble/LED report) that writes past the end of this
                // stack variable. Reusing the same usb_raw_control_io
                // wrapper (ep0 struct + a real EP0_MAX_DATA-sized buffer
                // immediately after it) used everywhere else in this
                // function fixes that.
                struct usb_raw_control_io read_io;
                memset(&read_io, 0, sizeof(read_io));
                read_io.inner.ep = 0;
                read_io.inner.length = ctrl->wLength;
                ioctl(dev->fd, USB_RAW_IOCTL_EP0_READ, &read_io);

                ack_zero_length_status(dev, "SET_REPORT");
                return true;
            }
            case 0x0a: { // SET_IDLE
                // Worth calling out specifically among the
                // ack_zero_length_status() call sites: Linux's own usbhid
                // driver issues SET_IDLE as part of its own probe() on
                // every HID interface it binds to, so getting this wrong
                // could well have been blocking driver binding even after
                // SET_CONFIGURATION itself was fixed to complete correctly.
                ack_zero_length_status(dev, "SET_IDLE");
                return true;
            }
        }
    }

    if (reply) {
        if (ctrl->wLength < io.inner.length) {
            io.inner.length = ctrl->wLength;
        }
        if (ctrl->bRequestType & USB_DIR_IN) {
            if (ioctl(dev->fd, USB_RAW_IOCTL_EP0_WRITE, &io) < 0) {
                perror("ioctl(USB_RAW_IOCTL_EP0_WRITE)");
            }
        } else {
            if (ioctl(dev->fd, USB_RAW_IOCTL_EP0_READ, &io) < 0) {
                perror("ioctl(USB_RAW_IOCTL_EP0_READ)");
            }
        }
    } else {
        ioctl(dev->fd, USB_RAW_IOCTL_EP0_STALL, 0);
    }
    return reply;
}

bool raw_gadget_init(struct RawGadgetDevice *dev, int type) {
    dev->fd = -1;
    dev->ep_in = -1;
    dev->ep_out = -1;
    dev->ep_in_addr = -1;
    dev->ep_out_addr = -1;
    dev->device_open = false;
    dev->target_type = type;
    dev->eps_enabled = false;
    dev->ep_out_thread_spawned = false;
    dev->ep_in_thread_spawned = false;
    dev->pending_report_len = 0;
    dev->report_pending = false;
    pthread_mutex_init(&dev->report_mutex, NULL);
    pthread_cond_init(&dev->report_cond, NULL);

    dev->fd = open("/dev/raw-gadget", O_RDWR | O_CLOEXEC);
    if (dev->fd < 0) {
        perror("Failed to open /dev/raw-gadget");
        return false;
    }

    struct usb_raw_init arg;
    strcpy((char *)&arg.driver_name[0], "dummy_udc");
    strcpy((char *)&arg.device_name[0], "dummy_udc.0");
    arg.speed = USB_SPEED_HIGH;

    if (ioctl(dev->fd, USB_RAW_IOCTL_INIT, &arg) < 0) {
        perror("ioctl(USB_RAW_IOCTL_INIT)");
        close(dev->fd);
        dev->fd = -1;
        return false;
    }

    int run_retry = 0;
    while (ioctl(dev->fd, USB_RAW_IOCTL_RUN, 0) < 0) {
        if (errno == EBUSY && run_retry < 5) {
            run_retry++;
            usleep(200000); // 200ms
            continue;
        }
        perror("ioctl(USB_RAW_IOCTL_RUN)");
        close(dev->fd);
        dev->fd = -1;
        return false;
    }

    process_eps_info(dev);

    dev->configured = false;
    dev->device_open = true;

    printf("Raw-Gadget emulation running on dummy_udc.0\n");
    DBG("raw_gadget_init: fd=%d, ep_in=%d (addr %d), ep_out=%d (addr %d)", 
        dev->fd, dev->ep_in, dev->ep_in_addr, dev->ep_out, dev->ep_out_addr);

    signal(SIGUSR1, dummy_signal_handler);

    if (pthread_create(&dev->ep0_thread, NULL, ep0_loop, dev) == 0) {
        dev->ep0_thread_spawned = true;
    } else {
        perror("Failed to spawn ep0_loop thread");
    }

    if (pthread_create(&dev->ep_out_thread, NULL, ep_out_loop, dev) == 0) {
        dev->ep_out_thread_spawned = true;
    } else {
        perror("Failed to spawn ep_out_loop thread");
    }

    if (pthread_create(&dev->ep_in_thread, NULL, ep_in_loop, dev) == 0) {
        dev->ep_in_thread_spawned = true;
    } else {
        perror("Failed to spawn ep_in_loop thread");
    }

    return true;
}

void raw_gadget_close(struct RawGadgetDevice *dev) {
    dev->configured = false;
    dev->device_open = false;
    if (dev->ep0_thread_spawned) {
        pthread_kill(dev->ep0_thread, SIGUSR1);
        pthread_join(dev->ep0_thread, NULL);
        dev->ep0_thread_spawned = false;
    }
    if (dev->ep_out_thread_spawned) {
        pthread_kill(dev->ep_out_thread, SIGUSR1);
        pthread_join(dev->ep_out_thread, NULL);
        dev->ep_out_thread_spawned = false;
    }
    if (dev->ep_in_thread_spawned) {
        // Unlike ep0_loop/ep_out_loop, this thread can be blocked in
        // either of two different places -- waiting on the condvar for
        // the next report, or inside the blocking EP_WRITE ioctl sending
        // one -- so both wake mechanisms are needed: SIGUSR1 interrupts
        // the ioctl (same as the other two threads), and signaling the
        // condvar (after device_open is already false, so the loop's
        // wait predicate sees it and exits) wakes it if it was waiting.
        pthread_kill(dev->ep_in_thread, SIGUSR1);
        pthread_mutex_lock(&dev->report_mutex);
        pthread_cond_signal(&dev->report_cond);
        pthread_mutex_unlock(&dev->report_mutex);
        pthread_join(dev->ep_in_thread, NULL);
        dev->ep_in_thread_spawned = false;
    }
    pthread_mutex_destroy(&dev->report_mutex);
    pthread_cond_destroy(&dev->report_cond);
    if (dev->eps_enabled) {
        if (dev->ep_in >= 0) ioctl(dev->fd, USB_RAW_IOCTL_EP_DISABLE, dev->ep_in);
        if (dev->ep_out >= 0) ioctl(dev->fd, USB_RAW_IOCTL_EP_DISABLE, dev->ep_out);
        dev->eps_enabled = false;
    }
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

static void* ep0_loop(void *arg) {
    struct RawGadgetDevice *dev = (struct RawGadgetDevice *)arg;

    while (dev->device_open) {
        struct usb_raw_control_event event;
        event.inner.type = 0;
        event.inner.length = sizeof(event.ctrl);
        int rv = ioctl(dev->fd, USB_RAW_IOCTL_EVENT_FETCH, &event);
        if (rv < 0) {
            if (errno == EINTR) {
                if (!dev->device_open) break;
                continue;
            }
            if (!dev->device_open) break;
            perror("ioctl(USB_RAW_IOCTL_EVENT_FETCH)");
            break;
        }

        if (event.inner.type == USB_RAW_EVENT_RESET) {
            DBG("Raw-gadget event: RESET");
            dev->configured = false;
            if (dev->eps_enabled) {
                if (dev->ep_in >= 0) ioctl(dev->fd, USB_RAW_IOCTL_EP_DISABLE, dev->ep_in);
                if (dev->ep_out >= 0) ioctl(dev->fd, USB_RAW_IOCTL_EP_DISABLE, dev->ep_out);
                dev->eps_enabled = false;
            }
            dev->ep_in = -1;
            dev->ep_out = -1;
        } else if (event.inner.type == USB_RAW_EVENT_CONTROL) {
            struct usb_ctrlrequest *ctrl = &event.ctrl;
            bool is_set_config = (ctrl->bRequestType == 0x00 && ctrl->bRequest == USB_REQ_SET_CONFIGURATION);
            bool replied = handle_control_request(dev, &event);
            if (is_set_config && replied) {
                dev->configured = true;
            }
        }
    }
    return NULL;
}

// Owns the actual (blocking) USB_RAW_IOCTL_EP_WRITE call -- see the
// report_mutex/report_cond fields' doc comment in the header for why this
// can't run on whatever thread calls raw_gadget_send_input_report().
static void* ep_in_loop(void *arg) {
    struct RawGadgetDevice *dev = (struct RawGadgetDevice *)arg;
    uint8_t local_buf[sizeof(dev->pending_report)];

    while (dev->device_open) {
        pthread_mutex_lock(&dev->report_mutex);
        while (dev->device_open && !dev->report_pending) {
            pthread_cond_wait(&dev->report_cond, &dev->report_mutex);
        }
        if (!dev->device_open) {
            pthread_mutex_unlock(&dev->report_mutex);
            break;
        }
        size_t len = dev->pending_report_len;
        memcpy(local_buf, dev->pending_report, len);
        dev->report_pending = false;
        pthread_mutex_unlock(&dev->report_mutex);

        if (!dev->configured || !dev->eps_enabled || dev->ep_in < 0) continue;

        struct usb_raw_ep_io *io = (struct usb_raw_ep_io *)malloc(sizeof(struct usb_raw_ep_io) + len);
        io->ep = dev->ep_in;
        io->flags = 0;
        io->length = len;
        memcpy(io->data, local_buf, len);

        int rv = ioctl(dev->fd, USB_RAW_IOCTL_EP_WRITE, io);
        if (rv < 0 && errno != EINTR) {
            perror("ioctl(USB_RAW_IOCTL_EP_WRITE)");
        }
        free(io);
    }
    return NULL;
}

// Stages the latest report for ep_in_loop to send and returns immediately
// -- never touches the fd/ioctl directly. Superseding a not-yet-sent report
// is fine: every report carries full controller state, not a delta, so the
// newest one staged is always the only one worth sending.
bool raw_gadget_send_input_report(struct RawGadgetDevice *dev, const uint8_t *data, size_t size) {
    if (!dev->configured || !dev->eps_enabled || dev->ep_in < 0) return false;
    if (size > sizeof(dev->pending_report)) return false;

    pthread_mutex_lock(&dev->report_mutex);
    memcpy(dev->pending_report, data, size);
    dev->pending_report_len = size;
    dev->report_pending = true;
    pthread_cond_signal(&dev->report_cond);
    pthread_mutex_unlock(&dev->report_mutex);

    return true;
}
