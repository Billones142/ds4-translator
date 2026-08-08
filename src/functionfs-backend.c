#include "functionfs-backend.h"
#include "descriptors.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

/* Debug logging — compile with -DDS4_DEBUG to enable */
#ifdef DS4_DEBUG
#  define DBG(fmt, ...) fprintf(stderr, "[ds4-debug] " fmt "\n", ##__VA_ARGS__)
#else
#  define DBG(fmt, ...) ((void)0)
#endif

static void dummy_signal_handler(int sig) {
    (void)sig; // Just to interrupt a blocking read()/write()
}

extern int phy_fd;
extern bool is_bluetooth;
extern uint8_t cur_motor_left, cur_motor_right;
extern uint8_t cur_r, cur_g, cur_b;
extern void send_physical_output_report(int fd, bool bluetooth, uint8_t motor_left, uint8_t motor_right, uint8_t r, uint8_t g, uint8_t b);

#define GADGET_DIR "/sys/kernel/config/usb_gadget/ds4translatorffs"
#define FUNCTION_NAME "ds4emu0"
#define MOUNT_POINT "/dev/ffs-" FUNCTION_NAME

static void* ep0_loop(void *arg);
static void* in_loop(void *arg);
static void* out_loop(void *arg);

// Same data tables raw-gadget-backend.c's/hidg-backend.c's/main.cpp's
// GET_REPORT cases answer with -- see hidg-backend.c's build_feature_report()
// doc comment for why this isn't shared via a header.
static size_t build_feature_report(int target_type, uint8_t rnum, uint8_t *out) {
    memset(out, 0, 64);
    bool is_ds4 = (target_type == 1);
    if (is_ds4) {
        if (rnum == 0x02 || rnum == 0x25) {
            out[0] = rnum;
            static const uint8_t ds4_cal[36] = {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x04, 0x00, 0xfc, 0x00, 0x04,
                0x00, 0xfc, 0x00, 0x04, 0x00, 0xfc,
                0x00, 0x04, 0x00, 0x04, 0x00, 0x20,
                0x00, 0xe0, 0x00, 0x20, 0x00, 0xe0,
                0x00, 0x20, 0x00, 0xe0, 0x00, 0x00
            };
            memcpy(&out[1], ds4_cal, 36);
            return 37;
        } else if (rnum == 0x10 || rnum == 0x12) {
            out[0] = rnum;
            static const uint8_t mac_info[15] = {
                0xe8, 0x47, 0x3a, 0xd6, 0xe7, 0x74,
                0x08, 0x25, 0x00, 0x1e, 0x00, 0xee, 0x74, 0xd0, 0xbc
            };
            memcpy(&out[1], mac_info, 15);
            return 16;
        } else if (rnum == 0x31 || rnum == 0xa3) {
            out[0] = rnum;
            // See main.cpp's UHID_GET_REPORT 0xa3 case for the field layout
            // and why hw_version_major/fw_version_major need to be nonzero
            // for some titles' sanity checks.
            memcpy(&out[1], "Mar 25 2016", 11);
            memcpy(&out[17], "12:00:00", 8);
            out[33] = 0x00; out[34] = 0x01; // hw_version_major (0x0100)
            out[35] = 0x38; out[36] = 0x54; // hw_version_minor (captured)
            out[37] = 0x01; out[38] = 0x00; // fw_version_major (0x00000001)
            out[39] = 0x00; out[40] = 0x00;
            out[41] = 0x33; out[42] = 0x20; // fw_version_minor (captured)
            return 49;
        }
    } else { // DualSense
        if (rnum == 0x05) {
            static const uint8_t cal_data[41] = {
                0x05,
                0xff, 0xfc, 0xff, 0xfe, 0xff, 0x83, 0x22, 0x78,
                0xdd, 0x92, 0x22, 0x5f, 0xdd, 0x95, 0x22, 0x6d,
                0xdd, 0x1c, 0x02, 0x1c, 0x02, 0xf2, 0x1f, 0xed,
                0xdf, 0xe3, 0x20, 0xda, 0xe0, 0xee, 0x1f, 0xdf,
                0xdf, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            };
            memcpy(out, cal_data, 41);
            return 41;
        } else if (rnum == 0x20) {
            static const uint8_t fw_data[64] = {
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
            memcpy(out, fw_data, 64);
            return 64;
        } else if (rnum == 0x09) {
            static const uint8_t pairing_data[20] = {
                0x09,
                0xe8, 0x47, 0x3a, 0xd6, 0xe7, 0x74,
                0x08, 0x25, 0x00, 0x1e, 0x00, 0xee, 0x74, 0xd0, 0xbc,
                0x00, 0x00, 0x00, 0x00
            };
            memcpy(out, pairing_data, 20);
            return 20;
        }
    }
    return 0;
}

static bool write_file_str(const char *path, const char *content) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror(path);
        return false;
    }
    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    close(fd);
    if (written != (ssize_t)len) {
        fprintf(stderr, "Short write to %s\n", path);
        return false;
    }
    return true;
}

// mkdir() that treats "already exists" as success -- see hidg-backend.c's
// make_dir() doc comment (same rationale: leftover configfs dirs from an
// uncleanly-terminated previous run).
static bool make_dir(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) return true;
    perror(path);
    return false;
}

static void teardown_gadget_dir(void) {
    write_file_str(GADGET_DIR "/UDC", "");
    umount(MOUNT_POINT);
    rmdir(MOUNT_POINT);
    unlink(GADGET_DIR "/configs/c.1/" FUNCTION_NAME);
    rmdir(GADGET_DIR "/functions/ffs." FUNCTION_NAME);
    rmdir(GADGET_DIR "/configs/c.1/strings/0x409");
    rmdir(GADGET_DIR "/configs/c.1");
    rmdir(GADGET_DIR "/strings/0x409");
    rmdir(GADGET_DIR);
}

// Same rationale as hidg-backend.c's ensure_configfs_mounted() -- FunctionFS
// gadgets are also assembled through configfs's usb_gadget subsystem
// (libcomposite), the "ffs" function type is just a different function
// backend within the same framework.
static bool ensure_configfs_mounted(void) {
    struct stat st;
    if (stat("/sys/kernel/config", &st) != 0) {
        fprintf(stderr, "/sys/kernel/config does not exist -- is CONFIG_CONFIGFS_FS enabled?\n");
        return false;
    }
    if (stat("/sys/kernel/config/usb_gadget", &st) == 0) {
        return true;
    }
    if (mount("none", "/sys/kernel/config", "configfs", 0, NULL) != 0 && errno != EBUSY) {
        perror("mount configfs");
        return false;
    }
    if (stat("/sys/kernel/config/usb_gadget", &st) == 0) {
        return true;
    }
    fprintf(stderr, "/sys/kernel/config/usb_gadget does not exist -- is the libcomposite kernel module loaded? (modprobe libcomposite)\n");
    return false;
}

// Same rationale as hidg-backend.c's find_udc() -- prefers dummy_udc.0.
static bool find_udc(char *out, size_t out_size) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) {
        perror("opendir(/sys/class/udc)");
        return false;
    }
    struct dirent *entry;
    bool found = false;
    char first[256] = {0};
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!found) {
            snprintf(first, sizeof(first), "%s", entry->d_name);
        }
        if (strncmp(entry->d_name, "dummy_udc", 9) == 0) {
            snprintf(out, out_size, "%s", entry->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    if (found) return true;
    if (first[0] != '\0') {
        snprintf(out, out_size, "%s", first);
        return true;
    }
    fprintf(stderr, "No UDC found under /sys/class/udc -- is dummy_hcd loaded?\n");
    return false;
}

#define STR_INTERFACE_ "DS4 Emulation"

// Builds the V2-format descriptors blob FunctionFS expects written to ep0:
// header + fs_count + hs_count, then one HID interface (interface + HID
// class descriptor + 2 interrupt endpoints) per speed. Unlike raw-gadget,
// there's no separate DEVICE/CONFIG descriptor to build here -- the gadget
// core answers those itself from the configfs attributes/strings written in
// functionfs_init(), using this blob only to learn the function's own
// interface/endpoints.
static size_t build_ffs_descriptors(int target_type, uint8_t *buf) {
    uint16_t rdesc_len = (target_type == 1) ? (uint16_t)sizeof(ds4_usb_rdesc) : (uint16_t)sizeof(dualsense_usb_rdesc);

    uint8_t *p = buf;

    // Header (filled in after we know the total length)
    uint8_t *header = p;
    p += 20; // magic(4) + length(4) + flags(4) + fs_count(4) + hs_count(4)

    uint8_t *descs_start = p;
    for (int speed = 0; speed < 2; speed++) {
        // Interface
        p[0] = 9; p[1] = USB_DT_INTERFACE;
        p[2] = 0; p[3] = 0; // bInterfaceNumber, bAlternateSetting
        p[4] = 2; // bNumEndpoints
        p[5] = 3; // bInterfaceClass: HID
        p[6] = 0; p[7] = 0; // bInterfaceSubClass, bInterfaceProtocol
        p[8] = 1; // iInterface -> string index 1
        p += 9;

        // HID class descriptor
        p[0] = 9; p[1] = 0x21; // HID
        p[2] = 0x11; p[3] = 0x01; // bcdHID 1.11
        p[4] = 0; // bCountryCode
        p[5] = 1; // bNumDescriptors
        p[6] = 0x22; // Report
        p[7] = rdesc_len & 0xFF; p[8] = rdesc_len >> 8;
        p += 9;

        // Endpoint IN (1)
        p[0] = 7; p[1] = USB_DT_ENDPOINT;
        p[2] = USB_DIR_IN | 1;
        p[3] = 3; // Interrupt
        p[4] = 64; p[5] = 0; // wMaxPacketSize
        p[6] = 4; // bInterval
        p += 7;

        // Endpoint OUT (2)
        p[0] = 7; p[1] = USB_DT_ENDPOINT;
        p[2] = USB_DIR_OUT | 2;
        p[3] = 3; // Interrupt
        p[4] = 64; p[5] = 0;
        p[6] = 4;
        p += 7;
    }
    size_t descs_len = (size_t)(p - descs_start);
    size_t total_len = (size_t)(p - buf);

    uint32_t magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2;
    uint32_t flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC;
    uint32_t fs_count = 4, hs_count = 4;
    uint32_t total_len32 = (uint32_t)total_len;
    memcpy(header, &magic, 4);
    memcpy(header + 4, &total_len32, 4);
    memcpy(header + 8, &flags, 4);
    memcpy(header + 12, &fs_count, 4);
    memcpy(header + 16, &hs_count, 4);
    (void)descs_len;

    return total_len;
}

static size_t build_ffs_strings(uint8_t *buf) {
    uint8_t *p = buf;
    uint8_t *header = p;
    p += 16; // magic(4) + length(4) + str_count(4) + lang_count(4)

    uint16_t lang = 0x0409; // en-us
    memcpy(p, &lang, 2);
    p += 2;
    size_t str_len = strlen(STR_INTERFACE_) + 1;
    memcpy(p, STR_INTERFACE_, str_len);
    p += str_len;

    size_t total_len = (size_t)(p - buf);
    uint32_t magic = FUNCTIONFS_STRINGS_MAGIC;
    uint32_t total_len32 = (uint32_t)total_len;
    uint32_t str_count = 1, lang_count = 1;
    memcpy(header, &magic, 4);
    memcpy(header + 4, &total_len32, 4);
    memcpy(header + 8, &str_count, 4);
    memcpy(header + 12, &lang_count, 4);

    return total_len;
}

// Answers a single FUNCTIONFS_SETUP event on ep0. Only class (HID) requests
// and the interface's own GET_DESCRIPTOR(HID_REPORT) reach here -- standard
// device/config/string GET_DESCRIPTOR, SET_CONFIGURATION and SET_INTERFACE
// are all answered by the gadget core below FunctionFS before userspace
// ever sees them (see the header's doc comment on ep0_thread).
static void handle_setup(struct FunctionFSDevice *dev, const struct usb_ctrlrequest *setup) {
    uint8_t buf[64];
    DBG("FFS SETUP: reqType=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=%d",
        setup->bRequestType, setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);

    uint8_t req_type = setup->bRequestType & USB_TYPE_MASK;
    uint8_t recip = setup->bRequestType & USB_RECIP_MASK;
    bool dir_in = (setup->bRequestType & USB_DIR_IN) != 0;

    if (req_type == USB_TYPE_STANDARD && recip == USB_RECIP_INTERFACE &&
        setup->bRequest == USB_REQ_GET_DESCRIPTOR && (setup->wValue >> 8) == 0x22 /* HID Report */) {
        const uint8_t *rdesc = (dev->target_type == 1) ? ds4_usb_rdesc : dualsense_usb_rdesc;
        size_t rdesc_len = (dev->target_type == 1) ? sizeof(ds4_usb_rdesc) : sizeof(dualsense_usb_rdesc);
        size_t len = setup->wLength < rdesc_len ? setup->wLength : rdesc_len;
        if (write(dev->ep0_fd, rdesc, len) < 0) perror("ep0 write (report descriptor)");
        return;
    }

    if (req_type == USB_TYPE_CLASS && recip == USB_RECIP_INTERFACE) {
        if (setup->bRequest == 0x01 && dir_in) { // GET_REPORT
            uint8_t rtype = setup->wValue >> 8;
            uint8_t rnum = setup->wValue & 0xFF;
            size_t len = 0;
            if (rtype == 3) { // Feature
                len = build_feature_report(dev->target_type, rnum, buf);
            }
            if (len == 0) {
                buf[0] = rnum;
                len = 1;
            }
            if (len > setup->wLength) len = setup->wLength;
            if (write(dev->ep0_fd, buf, len) < 0) perror("ep0 write (GET_REPORT)");
            return;
        } else if (setup->bRequest == 0x09) { // SET_REPORT
            // No rumble/LED parsing here, same as raw-gadget-backend.c's
            // SET_REPORT case -- the interrupt OUT endpoint (out_loop) is
            // the path actually used for that; this just drains and acks
            // the control transfer's data stage so it doesn't stall.
            if (setup->wLength > 0) {
                ssize_t rv = read(dev->ep0_fd, buf, setup->wLength < sizeof(buf) ? setup->wLength : sizeof(buf));
                (void)rv;
            }
            return;
        } else if (setup->bRequest == 0x0a) { // SET_IDLE
            uint8_t ack = 0;
            if (write(dev->ep0_fd, &ack, dir_in ? 1 : 0) < 0 && dir_in) {
                // Some UDC/FunctionFS combinations expect a 1-byte ack here
                // even though SET_IDLE carries no host-visible data; matches
                // the equivalent case in senseshock's handle_setup_request().
                perror("ep0 write (SET_IDLE ack)");
            }
            return;
        }
    }
    // Unhandled: leave the control transfer unanswered. FunctionFS/the UDC
    // times it out on its own; this only happens for requests no title
    // exercised during development (matches senseshock's own fallthrough
    // behavior for the same class of request).
}

static void* ep0_loop(void *arg) {
    struct FunctionFSDevice *dev = (struct FunctionFSDevice *)arg;

    while (dev->device_open) {
        struct usb_functionfs_event event;
        ssize_t rv = read(dev->ep0_fd, &event, sizeof(event));
        if (rv < 0) {
            if (errno == EINTR) {
                if (!dev->device_open) break;
                continue;
            }
            if (!dev->device_open) break;
            usleep(10000);
            continue;
        }
        if (rv != (ssize_t)sizeof(event)) continue;

        switch (event.type) {
            case FUNCTIONFS_ENABLE:
                DBG("FFS event: ENABLE");
                dev->configured = true;
                break;
            case FUNCTIONFS_DISABLE:
                DBG("FFS event: DISABLE");
                dev->configured = false;
                break;
            case FUNCTIONFS_SETUP:
                handle_setup(dev, &event.u.setup);
                break;
            default:
                break;
        }
    }
    return NULL;
}

// Reads host->device OUT reports (rumble/LED). Byte layout matches
// raw-gadget-backend.c's ep_out_loop()/hidg-backend.c's out_loop() exactly
// -- all three backends serve the identical report descriptors from
// descriptors.h.
static void* out_loop(void *arg) {
    struct FunctionFSDevice *dev = (struct FunctionFSDevice *)arg;
    uint8_t data[64];

    while (dev->device_open) {
        if (!dev->configured) {
            usleep(10000);
            continue;
        }
        ssize_t rv = read(dev->ep_out_fd, data, sizeof(data));
        if (rv < 0) {
            if (errno == EINTR) {
                if (!dev->device_open) break;
                continue;
            }
            usleep(10000);
            continue;
        }
        if (rv <= 0) continue;

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
    return NULL;
}

// Owns the actual (blocking) write() -- see the report_mutex/report_cond
// fields' doc comment in the header for why this can't run on whatever
// thread calls functionfs_send_input_report().
static void* in_loop(void *arg) {
    struct FunctionFSDevice *dev = (struct FunctionFSDevice *)arg;
    uint8_t local_buf[sizeof(dev->pending_report)];
    int last_errno = 0; // see hidg-backend.c's in_loop() doc comment

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

        if (!dev->configured || dev->ep_in_fd < 0) continue;

        ssize_t rv = write(dev->ep_in_fd, local_buf, len);
        if (rv < 0 && errno != EINTR) {
            if (errno != last_errno) {
                perror("write(ep1)");
                last_errno = errno;
            }
            usleep(2000);
        } else if (rv >= 0) {
            last_errno = 0;
        }
    }
    return NULL;
}

bool functionfs_send_input_report(struct FunctionFSDevice *dev, const uint8_t *data, size_t size) {
    if (!dev->configured || dev->ep_in_fd < 0) return false;
    if (size > sizeof(dev->pending_report)) return false;

    pthread_mutex_lock(&dev->report_mutex);
    memcpy(dev->pending_report, data, size);
    dev->pending_report_len = size;
    dev->report_pending = true;
    pthread_cond_signal(&dev->report_cond);
    pthread_mutex_unlock(&dev->report_mutex);

    return true;
}

bool functionfs_init(struct FunctionFSDevice *dev, int target_type) {
    dev->ep0_fd = -1;
    dev->ep_in_fd = -1;
    dev->ep_out_fd = -1;
    dev->device_open = false;
    dev->configured = false;
    dev->target_type = target_type;
    dev->ep0_thread_spawned = false;
    dev->out_thread_spawned = false;
    dev->in_thread_spawned = false;
    dev->pending_report_len = 0;
    dev->report_pending = false;
    pthread_mutex_init(&dev->report_mutex, NULL);
    pthread_cond_init(&dev->report_cond, NULL);

    if (!ensure_configfs_mounted()) return false;

    teardown_gadget_dir();
    if (!make_dir(GADGET_DIR)) return false;

    bool is_ds4 = (target_type == 1);

    bool ok = true;
    ok = ok && write_file_str(GADGET_DIR "/idVendor", "0x054c\n");
    ok = ok && write_file_str(GADGET_DIR "/idProduct", is_ds4 ? "0x05c4\n" : "0x0ce6\n");
    ok = ok && write_file_str(GADGET_DIR "/bcdDevice", "0x8111\n");
    ok = ok && write_file_str(GADGET_DIR "/bcdUSB", "0x0200\n");

    ok = ok && make_dir(GADGET_DIR "/strings/0x409");
    ok = ok && write_file_str(GADGET_DIR "/strings/0x409/manufacturer",
                               is_ds4 ? "Sony Computer Entertainment\n" : "Sony Interactive Entertainment\n");
    ok = ok && write_file_str(GADGET_DIR "/strings/0x409/product", "Wireless Controller\n");
    ok = ok && write_file_str(GADGET_DIR "/strings/0x409/serialnumber", "74:e7:d6:3a:47:e8\n");

    ok = ok && make_dir(GADGET_DIR "/configs/c.1");
    ok = ok && make_dir(GADGET_DIR "/configs/c.1/strings/0x409");
    ok = ok && write_file_str(GADGET_DIR "/configs/c.1/strings/0x409/configuration", "Default Configuration\n");
    ok = ok && write_file_str(GADGET_DIR "/configs/c.1/MaxPower", "500\n");
    ok = ok && write_file_str(GADGET_DIR "/configs/c.1/bmAttributes", "0x80\n"); // bus-powered

    ok = ok && make_dir(GADGET_DIR "/functions/ffs." FUNCTION_NAME);

    if (!ok) {
        teardown_gadget_dir();
        return false;
    }

    if (symlink(GADGET_DIR "/functions/ffs." FUNCTION_NAME, GADGET_DIR "/configs/c.1/" FUNCTION_NAME) != 0 && errno != EEXIST) {
        perror("symlink ffs function into config");
        teardown_gadget_dir();
        return false;
    }

    if (!make_dir(MOUNT_POINT)) {
        teardown_gadget_dir();
        return false;
    }
    if (mount(FUNCTION_NAME, MOUNT_POINT, "functionfs", 0, NULL) != 0) {
        perror("mount functionfs");
        teardown_gadget_dir();
        return false;
    }

    dev->ep0_fd = open(MOUNT_POINT "/ep0", O_RDWR);
    if (dev->ep0_fd < 0) {
        perror(MOUNT_POINT "/ep0");
        teardown_gadget_dir();
        return false;
    }

    uint8_t descs_buf[256];
    size_t descs_len = build_ffs_descriptors(target_type, descs_buf);
    if (write(dev->ep0_fd, descs_buf, descs_len) < 0) {
        perror("write ep0 (descriptors)");
        teardown_gadget_dir();
        return false;
    }

    uint8_t strings_buf[64];
    size_t strings_len = build_ffs_strings(strings_buf);
    if (write(dev->ep0_fd, strings_buf, strings_len) < 0) {
        perror("write ep0 (strings)");
        teardown_gadget_dir();
        return false;
    }

    dev->ep_in_fd = open(MOUNT_POINT "/ep1", O_RDWR);
    dev->ep_out_fd = open(MOUNT_POINT "/ep2", O_RDWR);
    if (dev->ep_in_fd < 0 || dev->ep_out_fd < 0) {
        perror(MOUNT_POINT "/ep1 or ep2");
        teardown_gadget_dir();
        return false;
    }

    char udc_name[256];
    if (!find_udc(udc_name, sizeof(udc_name))) {
        teardown_gadget_dir();
        return false;
    }
    char udc_line[300];
    snprintf(udc_line, sizeof(udc_line), "%s\n", udc_name);
    if (!write_file_str(GADGET_DIR "/UDC", udc_line)) {
        teardown_gadget_dir();
        return false;
    }

    dev->device_open = true;

    printf("FunctionFS emulation running on %s (UDC %s)\n", MOUNT_POINT, udc_name);
    DBG("functionfs_init: ep0=%d ep1=%d ep2=%d", dev->ep0_fd, dev->ep_in_fd, dev->ep_out_fd);

    // sigaction, not signal(): see hidg-backend.c's identical comment --
    // glibc's signal() defaults to SA_RESTART, which would swallow the
    // SIGUSR1 used to break these threads out of their blocking read()/write().
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dummy_signal_handler;
    sigaction(SIGUSR1, &sa, NULL);

    if (pthread_create(&dev->ep0_thread, NULL, ep0_loop, dev) == 0) {
        dev->ep0_thread_spawned = true;
    } else {
        perror("Failed to spawn functionfs ep0_loop thread");
    }
    if (pthread_create(&dev->out_thread, NULL, out_loop, dev) == 0) {
        dev->out_thread_spawned = true;
    } else {
        perror("Failed to spawn functionfs out_loop thread");
    }
    if (pthread_create(&dev->in_thread, NULL, in_loop, dev) == 0) {
        dev->in_thread_spawned = true;
    } else {
        perror("Failed to spawn functionfs in_loop thread");
    }

    return true;
}

void functionfs_close(struct FunctionFSDevice *dev) {
    dev->configured = false;
    dev->device_open = false;
    if (dev->ep0_thread_spawned) {
        pthread_kill(dev->ep0_thread, SIGUSR1);
        pthread_join(dev->ep0_thread, NULL);
        dev->ep0_thread_spawned = false;
    }
    if (dev->out_thread_spawned) {
        pthread_kill(dev->out_thread, SIGUSR1);
        pthread_join(dev->out_thread, NULL);
        dev->out_thread_spawned = false;
    }
    if (dev->in_thread_spawned) {
        pthread_kill(dev->in_thread, SIGUSR1);
        pthread_mutex_lock(&dev->report_mutex);
        pthread_cond_signal(&dev->report_cond);
        pthread_mutex_unlock(&dev->report_mutex);
        pthread_join(dev->in_thread, NULL);
        dev->in_thread_spawned = false;
    }
    pthread_mutex_destroy(&dev->report_mutex);
    pthread_cond_destroy(&dev->report_cond);
    if (dev->ep0_fd >= 0) { close(dev->ep0_fd); dev->ep0_fd = -1; }
    if (dev->ep_in_fd >= 0) { close(dev->ep_in_fd); dev->ep_in_fd = -1; }
    if (dev->ep_out_fd >= 0) { close(dev->ep_out_fd); dev->ep_out_fd = -1; }
    teardown_gadget_dir();
}
