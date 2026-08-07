#include "hidg-backend.h"
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
#include <sys/ioctl.h>
#include <linux/usb/g_hid.h>

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

#define GADGET_DIR "/sys/kernel/config/usb_gadget/ds4translator"

static void* out_loop(void *arg);
static void* in_loop(void *arg);
static void* get_report_loop(void *arg);

// Same data tables main.cpp's UHID_GET_REPORT handler and raw-gadget-backend.c's
// GET_REPORT case answer with -- single source of truth would mean threading
// this through a shared header, but the three backends' surrounding call
// shapes (uhid_event / usb_raw_control_io / usb_hidg_report) are different
// enough that they've each carried their own copy from the start; matching
// that existing pattern instead of a first-of-its-kind refactor here.
// Returns report length, or 0 if this rnum isn't one we answer (caller must
// still reply -- an unanswered GET_REPORT leaves the host's control transfer
// stalled waiting).
static size_t build_feature_report(int target_type, uint8_t rnum, uint8_t *out) {
    memset(out, 0, MAX_REPORT_LENGTH);
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
            out[35] = 0x38; out[36] = 0x54;
            out[41] = 0x33; out[42] = 0x20;
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

static bool write_file_bin(const char *path, const uint8_t *data, size_t len) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror(path);
        return false;
    }
    ssize_t written = write(fd, data, len);
    close(fd);
    if (written != (ssize_t)len) {
        fprintf(stderr, "Short write to %s\n", path);
        return false;
    }
    return true;
}

// mkdir() that treats "already exists" as success -- configfs directories
// created by a previous, uncleanly-terminated run (crash, kill -9) are left
// behind on disk, so init has to tolerate re-creating on top of them.
static bool make_dir(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) return true;
    perror(path);
    return false;
}

// Best-effort configfs teardown of a previous run's leftover gadget dir
// (see make_dir()'s doc comment) before building a fresh one. Ignores
// failures -- if nothing is there, every call below just fails ENOENT.
static void teardown_gadget_dir(void) {
    write_file_str(GADGET_DIR "/UDC", "");
    unlink(GADGET_DIR "/configs/c.1/hid.usb0");
    rmdir(GADGET_DIR "/functions/hid.usb0");
    rmdir(GADGET_DIR "/configs/c.1/strings/0x409");
    rmdir(GADGET_DIR "/configs/c.1");
    rmdir(GADGET_DIR "/strings/0x409");
    rmdir(GADGET_DIR);
}

static bool ensure_configfs_mounted(void) {
    struct stat st;
    if (stat("/sys/kernel/config", &st) != 0) {
        fprintf(stderr, "/sys/kernel/config does not exist -- is CONFIG_CONFIGFS_FS enabled?\n");
        return false;
    }
    if (stat("/sys/kernel/config/usb_gadget", &st) == 0) {
        return true; // already mounted with the gadget subsystem present
    }
    if (mount("none", "/sys/kernel/config", "configfs", 0, NULL) != 0 && errno != EBUSY) {
        perror("mount configfs");
        return false;
    }
    // configfs itself is mounted now (either just now, or EBUSY meaning it
    // already was) -- but that's not the same thing as the usb_gadget
    // subsystem existing under it. usb_gadget is registered by the
    // libcomposite module specifically; mounting configfs again does
    // nothing to fix a missing/unloaded libcomposite, so check for real
    // rather than assuming EBUSY meant "good to go" (which previously made
    // this function report success right before mkdir(GADGET_DIR) failed
    // with a confusing ENOENT further down).
    if (stat("/sys/kernel/config/usb_gadget", &st) == 0) {
        return true;
    }
    fprintf(stderr, "/sys/kernel/config/usb_gadget does not exist -- is the libcomposite kernel module loaded? (modprobe libcomposite)\n");
    return false;
}

// Find a free UDC to bind the gadget to. Prefers dummy_udc.0 (the same one
// raw-gadget-backend.c binds to) since that's what dummy_hcd registers, but
// falls back to whatever else is listed so a real UDC still works if present.
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
        // snprintf, not strncpy: strncpy doesn't guarantee null-termination
        // when the source is >= the destination size, which is exactly the
        // "silent truncation" class of bug this Makefile's strict warnings
        // now catch (-Wstringop-truncation) -- snprintf always terminates.
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

// After binding UDC, resolve which /dev/hidgN node the kernel created for
// our hid.usb0 function by reading its "dev" attribute ("major:minor") and
// following /sys/dev/char/<major>:<minor> back to a device name, rather
// than assuming /dev/hidg0 (which would be wrong if another gadget/hidg
// function already exists on the system).
static bool resolve_hidg_path(char *out, size_t out_size) {
    FILE *f = fopen(GADGET_DIR "/functions/hid.usb0/dev", "r");
    if (!f) {
        perror(GADGET_DIR "/functions/hid.usb0/dev");
        return false;
    }
    unsigned major = 0, minor = 0;
    int n = fscanf(f, "%u:%u", &major, &minor);
    fclose(f);
    if (n != 2) {
        fprintf(stderr, "Failed to parse hid.usb0/dev\n");
        return false;
    }

    char sys_path[64];
    snprintf(sys_path, sizeof(sys_path), "/sys/dev/char/%u:%u", major, minor);
    char link_target[512];
    ssize_t len = readlink(sys_path, link_target, sizeof(link_target) - 1);
    if (len < 0) {
        perror(sys_path);
        return false;
    }
    link_target[len] = '\0';

    const char *name = strrchr(link_target, '/');
    name = name ? name + 1 : link_target;

    // Reject rather than silently truncate: a truncated path here would
    // point hidg_init() at a different, wrong /dev node instead of failing
    // loudly, which is a worse outcome than just refusing to start.
    size_t needed = strlen("/dev/") + strlen(name) + 1;
    if (needed > out_size) {
        fprintf(stderr, "hidg device name too long for buffer: %s\n", name);
        return false;
    }
    snprintf(out, out_size, "/dev/%s", name);
    return true;
}

bool hidg_init(struct HidgDevice *dev, int target_type) {
    dev->fd = -1;
    dev->device_open = false;
    dev->configured = false;
    dev->target_type = target_type;
    dev->out_thread_spawned = false;
    dev->in_thread_spawned = false;
    dev->get_report_thread_spawned = false;
    dev->pending_report_len = 0;
    dev->report_pending = false;
    pthread_mutex_init(&dev->report_mutex, NULL);
    pthread_cond_init(&dev->report_cond, NULL);

    if (!ensure_configfs_mounted()) return false;

    teardown_gadget_dir();
    if (!make_dir(GADGET_DIR)) return false;

    bool is_ds4 = (target_type == 1);
    const uint8_t *rdesc = is_ds4 ? ds4_usb_rdesc : dualsense_usb_rdesc;
    size_t rdesc_len = is_ds4 ? sizeof(ds4_usb_rdesc) : sizeof(dualsense_usb_rdesc);

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
    ok = ok && write_file_str(GADGET_DIR "/configs/c.1/strings/0x409/configuration", "DS4 Translator\n");
    ok = ok && write_file_str(GADGET_DIR "/configs/c.1/MaxPower", "500\n");

    ok = ok && make_dir(GADGET_DIR "/functions/hid.usb0");
    ok = ok && write_file_str(GADGET_DIR "/functions/hid.usb0/protocol", "0\n");
    ok = ok && write_file_str(GADGET_DIR "/functions/hid.usb0/subclass", "0\n");
    ok = ok && write_file_str(GADGET_DIR "/functions/hid.usb0/report_length", "64\n");
    ok = ok && write_file_bin(GADGET_DIR "/functions/hid.usb0/report_desc", rdesc, rdesc_len);

    if (!ok) {
        teardown_gadget_dir();
        return false;
    }

    if (symlink(GADGET_DIR "/functions/hid.usb0", GADGET_DIR "/configs/c.1/hid.usb0") != 0 && errno != EEXIST) {
        perror("symlink hid.usb0 into config");
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

    char hidg_path[64];
    if (!resolve_hidg_path(hidg_path, sizeof(hidg_path))) {
        write_file_str(GADGET_DIR "/UDC", "");
        teardown_gadget_dir();
        return false;
    }

    dev->fd = open(hidg_path, O_RDWR | O_CLOEXEC);
    if (dev->fd < 0) {
        perror(hidg_path);
        write_file_str(GADGET_DIR "/UDC", "");
        teardown_gadget_dir();
        return false;
    }

    dev->configured = true;
    dev->device_open = true;

    printf("HIDG emulation running on %s (UDC %s)\n", hidg_path, udc_name);
    DBG("hidg_init: fd=%d path=%s", dev->fd, hidg_path);

    // sigaction, not signal(): glibc's signal() installs handlers with
    // SA_RESTART (BSD semantics), which would auto-restart out_loop's
    // blocking read() right past the SIGUSR1 -- pthread_join in hidg_close()
    // would then hang forever waiting for a thread that never sees
    // device_open go false. sa_flags=0 gets us real EINTR.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dummy_signal_handler;
    sigaction(SIGUSR1, &sa, NULL);

    if (pthread_create(&dev->out_thread, NULL, out_loop, dev) == 0) {
        dev->out_thread_spawned = true;
    } else {
        perror("Failed to spawn hidg out_loop thread");
    }

    if (pthread_create(&dev->in_thread, NULL, in_loop, dev) == 0) {
        dev->in_thread_spawned = true;
    } else {
        perror("Failed to spawn hidg in_loop thread");
    }

    if (pthread_create(&dev->get_report_thread, NULL, get_report_loop, dev) == 0) {
        dev->get_report_thread_spawned = true;
    } else {
        perror("Failed to spawn hidg get_report_loop thread");
    }

    return true;
}

void hidg_close(struct HidgDevice *dev) {
    dev->configured = false;
    dev->device_open = false;
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
    if (dev->get_report_thread_spawned) {
        pthread_kill(dev->get_report_thread, SIGUSR1);
        pthread_join(dev->get_report_thread, NULL);
        dev->get_report_thread_spawned = false;
    }
    pthread_mutex_destroy(&dev->report_mutex);
    pthread_cond_destroy(&dev->report_cond);
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
    write_file_str(GADGET_DIR "/UDC", "");
    teardown_gadget_dir();
}

// Reads host->device OUT reports (rumble/LED) off the same /dev/hidgN node.
// Byte layout matches raw-gadget-backend.c's ep_out_loop() exactly -- both
// backends serve the identical report descriptors from descriptors.h.
static void* out_loop(void *arg) {
    struct HidgDevice *dev = (struct HidgDevice *)arg;
    uint8_t data[64];

    while (dev->device_open) {
        ssize_t rv = read(dev->fd, data, sizeof(data));
        if (rv < 0) {
            if (errno == EINTR) {
                if (!dev->device_open) break;
                continue;
            }
            usleep(10000);
            continue;
        }
        if (rv == 0) continue;

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
// thread calls hidg_send_input_report().
static void* in_loop(void *arg) {
    struct HidgDevice *dev = (struct HidgDevice *)arg;
    uint8_t local_buf[sizeof(dev->pending_report)];
    // Physical input reports arrive far faster than USB (re-)enumeration
    // completes (or than a torn-down endpoint gets noticed). Without this,
    // a failing write() gets retried on every single physical report --
    // hundreds/sec -- which both floods the log and hammers the gadget's
    // endpoint request queue hard enough to have caused a real
    // "End Point Request ERROR: -108" / disconnect in testing.
    int last_errno = 0;

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

        if (dev->fd < 0) continue;

        ssize_t rv = write(dev->fd, local_buf, len);
        if (rv < 0 && errno != EINTR) {
            if (errno != last_errno) {
                perror("write(/dev/hidgN)");
                last_errno = errno;
            }
            usleep(2000);
        } else if (rv >= 0) {
            last_errno = 0;
        }
    }
    return NULL;
}

// Answers control-transfer GET_REPORT (feature reports). f_hid stalls the
// pending control transfer on the host side until this replies -- unlike
// OUT reports there's no interrupt-endpoint fallback, so this is the only
// way a hidg-backed device can ever pass hid-playstation's/hid-sony's
// probe(), which synchronously GET_REPORTs calibration/pairing data before
// it will create the gamepad input device (see the "Failed to retrieve
// DualShock4 pairing info" kernel log this was written to fix).
static void* get_report_loop(void *arg) {
    struct HidgDevice *dev = (struct HidgDevice *)arg;

    while (dev->device_open) {
        __u8 report_id = 0;
        int rv = ioctl(dev->fd, GADGET_HID_READ_GET_REPORT_ID, &report_id);
        if (rv < 0) {
            if (errno == EINTR) {
                if (!dev->device_open) break;
                continue;
            }
            if (errno == ENOTTY || errno == ENOSYS || errno == EOPNOTSUPP) {
                // Permanent, not transient -- this kernel's f_hid predates
                // the polled GET_REPORT ioctl (added ~2022; exact mainline
                // version unconfirmed). Retrying can't ever succeed, so
                // stop instead of spinning a thread at 100Hz for nothing.
                // Feature reports will get f_hid's built-in zero/stall
                // default -- the disclosed hidg limitation, not a bug.
                fprintf(stderr, "GADGET_HID_READ_GET_REPORT_ID not supported by this kernel "
                                 "-- feature reports (calibration/pairing info) will not be answered.\n");
                break;
            }
            usleep(10000);
            continue;
        }

        struct usb_hidg_report resp;
        memset(&resp, 0, sizeof(resp));
        resp.report_id = report_id;
        resp.userspace_req = 1; // answer only this pending request, not future ones -- report contents (e.g. calibration) are static per-id here, but staying in the polled-response model keeps this consistent with how a real controller would answer each GET_REPORT freshly
        size_t len = build_feature_report(dev->target_type, report_id, resp.data);
        resp.length = (__u16)len;
        DBG("GET_REPORT id=0x%02x -> %zu bytes", report_id, len);

        if (ioctl(dev->fd, GADGET_HID_WRITE_GET_REPORT, &resp) < 0 && errno != EINTR) {
            perror("ioctl(GADGET_HID_WRITE_GET_REPORT)");
        }
    }
    return NULL;
}

// Stages the latest report for in_loop to send and returns immediately --
// never touches the fd directly. Superseding a not-yet-sent report is fine:
// every report carries full controller state, not a delta, so the newest
// one staged is always the only one worth sending.
bool hidg_send_input_report(struct HidgDevice *dev, const uint8_t *data, size_t size) {
    if (!dev->configured || dev->fd < 0) return false;
    if (size > sizeof(dev->pending_report)) return false;

    pthread_mutex_lock(&dev->report_mutex);
    memcpy(dev->pending_report, data, size);
    dev->pending_report_len = size;
    dev->report_pending = true;
    pthread_cond_signal(&dev->report_cond);
    pthread_mutex_unlock(&dev->report_mutex);

    return true;
}
