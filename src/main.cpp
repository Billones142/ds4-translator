#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <linux/uhid.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "descriptors.h"
#include "raw-gadget-backend.h"

#ifndef DS4_VERSION
#define DS4_VERSION "unknown"
#endif

namespace fs = std::filesystem;

enum ControllerType {
    TYPE_DS4,
    TYPE_DUALSENSE,
    TYPE_NONE,
    TYPE_HIDDEN
};

// TYPE_DS4/TYPE_DUALSENSE are the only types that actually drive a virtual
// device; TYPE_NONE (fully hands-off passthrough) and TYPE_HIDDEN (physical
// hidden from other apps, but no translation) never create one.
bool controller_type_emulates(ControllerType type) {
    return type == TYPE_DS4 || type == TYPE_DUALSENSE;
}

// Every type except TYPE_NONE hides the physical controller from other apps.
bool controller_type_hides_physical(ControllerType type) {
    return type != TYPE_NONE;
}

const char* controller_type_name(ControllerType type) {
    switch (type) {
        case TYPE_DS4: return "DualShock 4";
        case TYPE_DUALSENSE: return "DualSense";
        case TYPE_HIDDEN: return "Hidden";
        default: return "None";
    }
}

const char* controller_type_config_str(ControllerType type) {
    switch (type) {
        case TYPE_DS4: return "ds4";
        case TYPE_DUALSENSE: return "dualsense";
        case TYPE_HIDDEN: return "hidden";
        default: return "none";
    }
}

struct PhysicalNode {
    std::string path;
    int fd;
    mode_t orig_mode;
    bool is_grabbed;
};

std::atomic<bool> running(true);

void signal_handler(int sig) {
    running = false;
}

// CRC32 calculation matching Linux kernel (IEEE 802.3 Ethernet polynomial 0xEDB88320)
uint32_t calculate_crc32(uint8_t seed, const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool table_initialized = false;
    if (!table_initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                if (c & 1) {
                    c = 0xEDB88320L ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[i] = c;
        }
        table_initialized = true;
    }

    uint32_t crc = 0xFFFFFFFF;
    crc = table[(crc ^ seed) & 0xFF] ^ (crc >> 8);
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Structures matching hid-playstation.c layout
struct __attribute__((packed)) dualshock4_input_report_common {
    uint8_t x, y;
    uint8_t rx, ry;
    uint8_t buttons[3];
    uint8_t z, rz;
    uint16_t sensor_timestamp;
    uint8_t sensor_temperature;
    int16_t gyro[3];
    int16_t accel[3];
    uint8_t reserved2[5];
    uint8_t status[2];
    uint8_t reserved3;
};

struct __attribute__((packed)) dualshock4_touch_point {
    uint8_t contact;
    uint8_t x_lo;
    uint8_t x_hi:4, y_lo:4;
    uint8_t y_hi;
};

struct __attribute__((packed)) dualshock4_touch_report {
    uint8_t timestamp;
    struct dualshock4_touch_point points[2];
};

struct __attribute__((packed)) dualshock4_input_report_bt {
    uint8_t report_id; // 0x11
    uint8_t reserved[2];
    struct dualshock4_input_report_common common;
    uint8_t num_touch_reports;
    struct dualshock4_touch_report touch_reports[4];
    uint8_t reserved2[2];
    uint32_t crc32;
};

struct __attribute__((packed)) dualshock4_input_report_usb {
    uint8_t report_id; // 0x01
    struct dualshock4_input_report_common common;
    uint8_t num_touch_reports;
    struct dualshock4_touch_report touch_reports[3];
    uint8_t reserved[3];
};

struct __attribute__((packed)) dualsense_touch_point {
    uint8_t contact;
    uint8_t x_lo;
    uint8_t x_hi:4, y_lo:4;
    uint8_t y_hi;
};

struct __attribute__((packed)) dualsense_input_report {
    uint8_t x, y;
    uint8_t rx, ry;
    uint8_t z, rz;
    uint8_t seq_number;
    uint8_t buttons[4];
    uint8_t reserved[4];
    int16_t gyro[3];
    int16_t accel[3];
    uint32_t sensor_timestamp;
    uint8_t reserved2;
    struct dualsense_touch_point points[2];
    uint8_t reserved3[12];
    uint8_t status[3];
    uint8_t reserved4[8];
};

// ---------- Standalone virtual controller (no physical hardware required) ----------
// Lets `ds4-ctl virtual` create and drive the UHID device directly over the
// IPC socket, so detection/compatibility can be tested without an actual
// DualShock 4/DualSense plugged in.
bool standalone_virtual = false;

// Set by the `release-physical` IPC command so the auto-scan loop stops
// reclaiming a physical controller that's still plugged in, letting a
// standalone virtual device be created/driven in its place for testing
// (e.g. the wine-controller-probe harness comparing real vs. emulated
// input delivery without needing the cable unplugged). Cleared by
// `resume-physical`.
bool ignore_physical = false;

enum {
    SYN_BTN_SQUARE   = 1 << 0,
    SYN_BTN_CROSS    = 1 << 1,
    SYN_BTN_CIRCLE   = 1 << 2,
    SYN_BTN_TRIANGLE = 1 << 3,
    SYN_BTN_L1       = 1 << 4,
    SYN_BTN_R1       = 1 << 5,
    SYN_BTN_L2       = 1 << 6,
    SYN_BTN_R2       = 1 << 7,
    SYN_BTN_SHARE    = 1 << 8,
    SYN_BTN_OPTIONS  = 1 << 9,
    SYN_BTN_L3       = 1 << 10,
    SYN_BTN_R3       = 1 << 11,
    SYN_BTN_PS       = 1 << 12,
    SYN_BTN_TOUCHPAD = 1 << 13,
};

struct SyntheticState {
    uint16_t buttons = 0;
    uint8_t dpad = 8; // HID hat switch: 0=up..7=up-left clockwise, 8=neutral
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    uint8_t l2_analog = 0, r2_analog = 0;
};
SyntheticState synth_state;

// Scan /dev/ for physical DualShock 4
std::string find_physical_ds4(bool& out_is_bluetooth) {
    std::string found_name = "";
    for (const auto& entry : fs::directory_iterator("/dev")) {
        std::string name = entry.path().filename().string();
        if (name.rfind("hidraw", 0) == 0) {
            std::string dev_path = "/dev/" + name;
            int fd = open(dev_path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd >= 0) {
                struct hidraw_devinfo info;
                if (ioctl(fd, HIDIOCGRAWINFO, &info) >= 0) {
                    if (info.vendor == 0x054c && (info.product == 0x05c4 || info.product == 0x09cc)) {
                        if (info.bustype == 0x05) { // BUS_BLUETOOTH
                            close(fd);
                            out_is_bluetooth = true;
                            return name;
                        } else {
                            found_name = name;
                            out_is_bluetooth = false;
                        }
                    }
                }
                close(fd);
            }
        }
    }
    return found_name;
}

// Find event nodes for a hidraw device
std::vector<std::string> get_event_nodes(const std::string& hidraw_name) {
    std::vector<std::string> event_nodes;
    std::string path = "/sys/class/hidraw/" + hidraw_name + "/device/input";
    if (fs::exists(path)) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_directory()) {
                for (const auto& subentry : fs::directory_iterator(entry.path())) {
                    std::string name = subentry.path().filename().string();
                    if (name.rfind("event", 0) == 0 || name.rfind("js", 0) == 0) {
                        event_nodes.push_back("/dev/input/" + name);
                    }
                }
            }
        }
    }
    return event_nodes;
}

// Forces the kernel to fully destroy and recreate the physical controller's
// hid/hidraw/input device nodes, equivalent to a real unplug+replug, by
// unbinding and rebinding its HID driver via sysfs. Needed on any type
// change that crosses the "is the physical controller hidden from other
// apps" boundary (TYPE_NONE <-> anything else), because neither direction
// is otherwise handled correctly for apps that are already running:
//  - Hiding it (chmod 0600 / EVIOCGRAB) only affects *future* open()
//    calls; a process (e.g. Steam) that already has the old hidraw node
//    open keeps reading raw reports from it regardless of permission
//    changes, until it closes and reopens the node itself.
//  - Un-hiding it doesn't generate a udev "add" event on its own (the
//    permission fixup via `udevadm trigger` only replays "change", since
//    the device was never actually removed) — most hotplug-aware apps,
//    Steam included, only rescan on "add"/"remove", so a newly-visible
//    device is never picked up until the app restarts.
// An actual unbind/bind cycle solves both: existing fds get ENODEV once
// the old node is destroyed, and the genuine "add" event on rebind is
// what makes hotplug-aware apps rescan and see it. This only touches the
// HID driver binding, not the underlying USB/Bluetooth connection, so it
// doesn't unpair or disconnect Bluetooth controllers.
//
// The `bind` write returning only means the kernel-side device object
// exists — it does NOT mean udevd has finished reacting to the resulting
// "add" uevent yet (running our rules, writing the real vendor/serial
// properties into the udev database). That happens asynchronously in
// userspace, so there's a real window right after bind where the device
// is already open-able but doesn't have its final properties applied.
// Steam has been observed racing exactly that window on the none->hidden
// direction, briefly reading a not-yet-fully-restored identity from it and
// caching a garbled/generic name until manually restarted. `udevadm
// settle` blocks until udev's event queue is empty, closing (not
// necessarily eliminating, since it can't stop a listener from reacting
// to the raw kernel uevent before udevd does) that window.
bool rebind_physical_hid_driver(const std::string& hidraw_name) {
    std::error_code ec;
    fs::path hid_dev = fs::canonical("/sys/class/hidraw/" + hidraw_name + "/device", ec);
    if (ec) return false;
    std::string hid_id = hid_dev.filename().string();

    fs::path driver_dir = fs::canonical(hid_dev / "driver", ec);
    if (ec) return false;

    std::ofstream unbind_f(driver_dir / "unbind");
    if (!unbind_f.is_open()) return false;
    unbind_f << hid_id;
    unbind_f.close();

    usleep(150000); // let the kernel fully tear down the old nodes

    std::ofstream bind_f(driver_dir / "bind");
    if (!bind_f.is_open()) return false;
    bind_f << hid_id;
    bind_f.close();

    system("udevadm settle --timeout=2");

    return true;
}

// Send output report to physical controller
extern "C" void send_physical_output_report(int fd, bool is_bluetooth, uint8_t motor_left, uint8_t motor_right, uint8_t r, uint8_t g, uint8_t b) {
    if (is_bluetooth) {
        uint8_t buf[78];
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x11; // Report ID
        buf[1] = 0xC0; // hw_control: HID | CRC32
        buf[2] = 0x00; // audio_control
        buf[3] = 0x07; // valid_flag0: motor | led | led_blink
        buf[4] = 0x00; // valid_flag1
        buf[5] = 0x00; // reserved
        buf[6] = motor_right;
        buf[7] = motor_left;
        buf[8] = r;
        buf[9] = g;
        buf[10] = b;
        
        uint32_t crc = calculate_crc32(0xA2, buf, 74);
        buf[74] = crc & 0xFF;
        buf[75] = (crc >> 8) & 0xFF;
        buf[76] = (crc >> 16) & 0xFF;
        buf[77] = (crc >> 24) & 0xFF;
        
        if (write(fd, buf, 78) < 0) {
            std::cerr << "Failed to write Bluetooth output report to physical controller" << std::endl;
        }
    } else {
        uint8_t buf[32];
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x05; // Report ID
        buf[1] = 0x07; // valid_flag0: motor | led | led_blink
        buf[2] = 0x00; // valid_flag1
        buf[3] = 0x00; // reserved
        buf[4] = motor_right;
        buf[5] = motor_left;
        buf[6] = r;
        buf[7] = g;
        buf[8] = b;
        
        if (write(fd, buf, 32) < 0) {
            std::cerr << "Failed to write USB output report to physical controller" << std::endl;
        }
    }
}

// UHID Helper to write events
int uhid_write(int fd, const struct uhid_event& ev) {
    ssize_t ret = write(fd, &ev, sizeof(ev));
    if (ret < 0) {
        std::cerr << "uhid write failed: " << strerror(errno) << std::endl;
        return -1;
    }
    return 0;
}

// Setup IPC Unix Domain Socket
int setup_ipc_socket() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Failed to create IPC socket: " << strerror(errno) << std::endl;
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/ds4-translator.sock", sizeof(addr.sun_path) - 1);

    unlink(addr.sun_path);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind IPC socket: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        std::cerr << "Failed to listen on IPC socket: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    // Set permissions to allow non-root status check/configuration
    chmod(addr.sun_path, 0666);

    return fd;
}

ControllerType read_config(ControllerType default_type) {
    std::ifstream f("/etc/ds4-translator.conf");
    if (!f.is_open()) {
        return default_type;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("type=", 0) == 0) {
            std::string val = line.substr(5);
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' ')) {
                val.pop_back();
            }
            if (val == "dualsense") {
                return TYPE_DUALSENSE;
            } else if (val == "ds4") {
                return TYPE_DS4;
            } else if (val == "none") {
                return TYPE_NONE;
            } else if (val == "hidden") {
                return TYPE_HIDDEN;
            }
        }
    }
    return default_type;
}

enum BackendType {
    BACKEND_UHID,
    BACKEND_GADGET
};

BackendType backend_type = BACKEND_UHID;
bool backend_explicitly_set = false;
RawGadgetDevice virtual_gadget = { .fd = -1, .ep_in = -1, .ep_out = -1, .device_open = false, .target_type = 0, .eps_enabled = false, .ep_out_thread_spawned = false, .configured = false };

extern "C" {
int phy_fd = -1;
int uhid_fd = -1;
bool is_bluetooth = false;
uint8_t cur_motor_left = 0, cur_motor_right = 0;
uint8_t cur_r = 0, cur_g = 0, cur_b = 255;
}
uint8_t sequence_number = 0;

// Last input state actually handed to the virtual device (physical
// passthrough or standalone synthetic), cached purely so `ds4-ctl test`
// can inspect what a game would currently see without needing its own
// separate read path into the report-forwarding logic below.
struct dualshock4_input_report_common last_virtual_common;
bool last_virtual_valid = false;

void write_config(ControllerType type) {
    std::ofstream f("/etc/ds4-translator.conf");
    if (f.is_open()) {
        f << "type=" << controller_type_config_str(type) << "\n";
    } else {
        std::cerr << "Failed to write config file: /etc/ds4-translator.conf: " << strerror(errno) << std::endl;
    }
}

bool create_virtual_device(ControllerType type) {
    if (!backend_explicitly_set) {
        backend_type = BACKEND_UHID;
    }

    if (backend_type == BACKEND_GADGET) {
        if (raw_gadget_init(&virtual_gadget, type == TYPE_DS4 ? 1 : 2)) {
            std::cout << "Virtual USB Controller created via Gadget." << std::endl;
            return true;
        } else {
            std::cerr << "Failed to initialize Gadget backend. Falling back to UHID backend..." << std::endl;
            backend_type = BACKEND_UHID;
        }
    }

    if (backend_type == BACKEND_UHID) {
        int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            std::cerr << "Failed to open /dev/uhid: " << strerror(errno) << std::endl;
            return false;
        }
        uhid_fd = fd;
        struct uhid_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = UHID_CREATE2;
        
        if (type == TYPE_DS4) {
            strncpy((char*)ev.u.create2.name, "Sony Computer Entertainment Wireless Controller", sizeof(ev.u.create2.name));
            strncpy((char*)ev.u.create2.uniq, "74:e7:d6:3a:47:e8", sizeof(ev.u.create2.uniq));
            ev.u.create2.rd_size = sizeof(ds4_usb_rdesc);
            memcpy(ev.u.create2.rd_data, ds4_usb_rdesc, sizeof(ds4_usb_rdesc));
            ev.u.create2.bus = BUS_USB;
            ev.u.create2.vendor = 0x054c;
            ev.u.create2.product = 0x05c4;
            ev.u.create2.version = 0x8111;
            ev.u.create2.country = 0;
        } else {
            strncpy((char*)ev.u.create2.name, "Sony Interactive Entertainment DualSense Wireless Controller", sizeof(ev.u.create2.name));
            strncpy((char*)ev.u.create2.uniq, "74:e7:d6:3a:47:e8", sizeof(ev.u.create2.uniq));
            ev.u.create2.rd_size = sizeof(dualsense_usb_rdesc);
            memcpy(ev.u.create2.rd_data, dualsense_usb_rdesc, sizeof(dualsense_usb_rdesc));
            ev.u.create2.bus = BUS_USB;
            ev.u.create2.vendor = 0x054c;
            ev.u.create2.product = 0x0ce6;
            ev.u.create2.version = 0x8111;
            ev.u.create2.country = 0;
        }

        if (uhid_write(uhid_fd, ev) < 0) {
            close(uhid_fd);
            uhid_fd = -1;
            return false;
        }
        std::cout << "Virtual USB Controller created via UHID." << std::endl;
        // Backgrounded: hid-playstation's probe() issues synchronous GET_REPORT
        // requests (calibration/pairing info) to this uhid device right after
        // creation. Blocking the main poll loop here (as a synchronous usleep +
        // system() previously did) starves those requests until they hit the
        // kernel's raw_request timeout (-EIO), which fails DS4 probe entirely
        // ("Failed to retrieve DualShock4 pairing info", "probe ... failed with
        // error -5") and leaves the device with no hidraw/input nodes at all.
        system("(sleep 0.25 && udevadm trigger) >/dev/null 2>&1 &");
        return true;
    }
    return false;
}

void destroy_virtual_device() {
    if (backend_type == BACKEND_GADGET) {
        raw_gadget_close(&virtual_gadget);
        usleep(200000); // Give kernel time to unbind dummy_udc.0
    } else {
        if (uhid_fd >= 0) {
            struct uhid_event destroy_ev;
            memset(&destroy_ev, 0, sizeof(destroy_ev));
            destroy_ev.type = UHID_DESTROY;
            uhid_write(uhid_fd, destroy_ev);
            close(uhid_fd);
            uhid_fd = -1;
            // Give hid-playstation time to unbind and the kernel to free the
            // old device's hidraw/event/js minor numbers before a
            // subsequent create_virtual_device() call. Without this, a fast
            // destroy-then-recreate cycle (e.g. unplug/replug, or type
            // change) can race the unbind: the kernel still considers the
            // old minors in use when the new device registers, so it hands
            // out the next free index (e.g. js1) instead of reusing the
            // one that just freed up (js0) — visible as the controller's
            // device index creeping upward across reconnects instead of
            // staying at the lowest available slot. Mirrors the same wait
            // already used for the gadget backend below.
            usleep(200000);
        }
    }
}

// Battery passthrough: the physical DS4's own status byte (common.status[0])
// already carries genuine charge/cable-state data from hardware, in the
// DS4 wire format (bit4 = cable connected, bits0-3 = level, with an
// out-of-range value of 11 used as a "fully charged" sentinel while wired).
// When emulating a DS4 target this can be forwarded byte-for-byte, but
// DualSense uses a different layout (bits0-3 = level, bits4-6 = charging
// state: 0=discharging, 1=charging, 2=full), so cross-emulating requires
// decoding to a state both formats can express and re-encoding.
enum BatteryState { BATTERY_DISCHARGING, BATTERY_CHARGING, BATTERY_FULL };

void decode_ds4_battery(uint8_t ds4_status0, int& percent, BatteryState& state) {
    uint8_t level = ds4_status0 & 0x0F;
    bool usb = (ds4_status0 & 0x10) != 0;
    if (usb) {
        if (level > 10) { percent = 100; state = BATTERY_FULL; }
        else { percent = level * 10; state = BATTERY_CHARGING; }
    } else {
        percent = (level > 8) ? 100 : (level + 1) * 10;
        state = BATTERY_DISCHARGING;
    }
    if (percent > 100) percent = 100;
}

uint8_t encode_dualsense_battery(int percent, BatteryState state) {
    uint8_t status_bits;
    switch (state) {
        case BATTERY_FULL:       status_bits = 0x2; break;
        case BATTERY_CHARGING:   status_bits = 0x1; break;
        default:                 status_bits = 0x0; break;
    }
    int level = (percent - 5) / 10;
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    return (uint8_t)((status_bits << 4) | (level & 0x0F));
}

// Build and send a UHID_INPUT2 report for `type` from a DS4-format "common"
// struct. Shared by the physical-passthrough path and the standalone
// synthetic-input path (`ds4-ctl virtual`), which differ only in how
// `common` gets populated (real hardware report vs. synthesized from
// keyboard-driven button state).
//
// `has_real_battery`: true only when `common` came from an actual physical
// controller's own report (its status byte already holds genuine charge/
// cable-state data — see decode_ds4_battery() above). The standalone
// virtual controller has no real battery to report, so it keeps reporting
// a fixed "fully charged, wired" state instead of a meaningless zero.
void emit_input_report(ControllerType type, const struct dualshock4_input_report_common& common,
                        uint8_t num_touch, const struct dualshock4_touch_report touch_reps[4],
                        bool has_real_battery) {
    last_virtual_common = common;
    last_virtual_valid = true;

    struct uhid_event out_ev;
    memset(&out_ev, 0, sizeof(out_ev));
    out_ev.type = UHID_INPUT2;

    if (type == TYPE_DS4) {
        out_ev.u.input2.size = sizeof(struct dualshock4_input_report_usb);
        struct dualshock4_input_report_usb* out_ds = (struct dualshock4_input_report_usb*)out_ev.u.input2.data;
        out_ds->report_id = 0x01;
        out_ds->common = common;
        if (!has_real_battery) {
            // No physical hardware behind this device — report a plausible
            // fixed state (USB-wired, fully charged) instead of whatever
            // zeroed/synthetic value `common.status` happens to hold.
            // status[0] bit4 = cable state (1=USB), bits 3:0 = battery level (0-10, 0x0B=full)
            out_ds->common.status[0] = 0x1B; // cable=1, battery=0x0B (full)
            out_ds->common.status[1] = 0x00;
        }
        // else: common.status[0]/[1] already came straight from the real
        // controller's own report via `out_ds->common = common` above.
        out_ds->num_touch_reports = (num_touch > 3) ? 3 : num_touch;
        for (int i = 0; i < out_ds->num_touch_reports; ++i) {
            out_ds->touch_reports[i] = touch_reps[i];
        }
        // Any remaining slots (all 3, when num_touch is 0 — every
        // synthetic/standalone report) must have both points marked "not
        // touching" (contact bit7 set). Real hardware always fills every
        // slot in the report regardless of how many actually carry a new
        // touch event; leaving them zeroed (contact=0x00, x=0, y=0) reads
        // as a genuine touch at (0,0) to any consumer that doesn't
        // strictly gate on num_touch_reports before reading slot contents.
        for (int i = out_ds->num_touch_reports; i < 3; ++i) {
            out_ds->touch_reports[i].points[0].contact = 0x80;
            out_ds->touch_reports[i].points[1].contact = 0x80;
        }
    } else { // DualSense
        out_ev.u.input2.size = 64;
        out_ev.u.input2.data[0] = 0x01;
        struct dualsense_input_report* out_ds5 = (struct dualsense_input_report*)&out_ev.u.input2.data[1];

        out_ds5->x = common.x;
        out_ds5->y = common.y;
        out_ds5->rx = common.rx;
        out_ds5->ry = common.ry;

        out_ds5->z = common.z;
        out_ds5->rz = common.rz;

        out_ds5->buttons[0] = common.buttons[0];
        out_ds5->buttons[1] = common.buttons[1];
        out_ds5->buttons[2] = common.buttons[2] & 0x03;
        out_ds5->buttons[3] = 0;

        out_ds5->seq_number = sequence_number++;
        memcpy(out_ds5->gyro, common.gyro, sizeof(out_ds5->gyro));
        memcpy(out_ds5->accel, common.accel, sizeof(out_ds5->accel));
        out_ds5->sensor_timestamp = (uint32_t)common.sensor_timestamp;

        if (num_touch > 0) {
            int latest = num_touch - 1;
            if (latest > 3) latest = 3;
            out_ds5->points[0].contact = touch_reps[latest].points[0].contact;
            out_ds5->points[0].x_lo = touch_reps[latest].points[0].x_lo;
            out_ds5->points[0].x_hi = touch_reps[latest].points[0].x_hi;
            out_ds5->points[0].y_lo = touch_reps[latest].points[0].y_lo;
            out_ds5->points[0].y_hi = touch_reps[latest].points[0].y_hi;

            out_ds5->points[1].contact = touch_reps[latest].points[1].contact;
            out_ds5->points[1].x_lo = touch_reps[latest].points[1].x_lo;
            out_ds5->points[1].x_hi = touch_reps[latest].points[1].x_hi;
            out_ds5->points[1].y_lo = touch_reps[latest].points[1].y_lo;
            out_ds5->points[1].y_hi = touch_reps[latest].points[1].y_hi;
        } else {
            out_ds5->points[0].contact = 0x80;
            out_ds5->points[1].contact = 0x80;
        }

        if (has_real_battery) {
            int percent;
            BatteryState state;
            decode_ds4_battery(common.status[0], percent, state);
            out_ds5->status[0] = encode_dualsense_battery(percent, state);
        } else {
            out_ds5->status[0] = 0x2B; // Fully charged, complete
        }
        out_ds5->status[1] = 0x00;
        out_ds5->status[2] = 0x00;
    }

    if (backend_type == BACKEND_GADGET) {
        raw_gadget_send_input_report(&virtual_gadget, out_ev.u.input2.data, out_ev.u.input2.size);
    } else {
        uhid_write(uhid_fd, out_ev);
    }
}

// Translate keyboard-driven synthetic button/axis state (`ds4-ctl virtual`)
// into the same DS4-format "common" struct the physical-passthrough path
// uses, so it can go through the identical emit_input_report() translation
// for either target type.
struct dualshock4_input_report_common build_synthetic_common(const SyntheticState& s) {
    struct dualshock4_input_report_common common;
    memset(&common, 0, sizeof(common));
    common.x = s.lx;  common.y = s.ly;
    common.rx = s.rx; common.ry = s.ry;
    common.z = s.l2_analog; common.rz = s.r2_analog;

    uint8_t b0 = (s.dpad & 0x0F);
    if (s.buttons & SYN_BTN_SQUARE)   b0 |= 0x10;
    if (s.buttons & SYN_BTN_CROSS)    b0 |= 0x20;
    if (s.buttons & SYN_BTN_CIRCLE)   b0 |= 0x40;
    if (s.buttons & SYN_BTN_TRIANGLE) b0 |= 0x80;

    uint8_t b1 = 0;
    if (s.buttons & SYN_BTN_L1)      b1 |= 0x01;
    if (s.buttons & SYN_BTN_R1)      b1 |= 0x02;
    if (s.buttons & SYN_BTN_L2)      b1 |= 0x04;
    if (s.buttons & SYN_BTN_R2)      b1 |= 0x08;
    if (s.buttons & SYN_BTN_SHARE)   b1 |= 0x10;
    if (s.buttons & SYN_BTN_OPTIONS) b1 |= 0x20;
    if (s.buttons & SYN_BTN_L3)      b1 |= 0x40;
    if (s.buttons & SYN_BTN_R3)      b1 |= 0x80;

    static uint8_t counter = 0;
    uint8_t b2 = (uint8_t)((counter++ & 0x3F) << 2);
    if (s.buttons & SYN_BTN_PS)       b2 |= 0x01;
    if (s.buttons & SYN_BTN_TOUCHPAD) b2 |= 0x02;

    common.buttons[0] = b0;
    common.buttons[1] = b1;
    common.buttons[2] = b2;
    return common;
}

// Sends one all-neutral report (no buttons held, sticks/triggers centered)
// before the virtual device goes away. Without this, whatever was last
// pressed at the moment of a disconnect (real controller unplugged, type
// switched, standalone virtual destroyed, daemon stopped) stays as the last
// report a game/Wine ever saw for that device — visible as a button
// appearing stuck "held" until the device is recreated and starts sending
// fresh reports again.
void emit_neutral_report(ControllerType type) {
    struct dualshock4_input_report_common neutral = build_synthetic_common(SyntheticState());
    struct dualshock4_touch_report empty_touch[4];
    memset(empty_touch, 0, sizeof(empty_touch));
    emit_input_report(type, neutral, 0, empty_touch, false);
}

int main(int argc, char* argv[]) {
    ControllerType target_type = TYPE_DS4;
    bool type_explicitly_set = false;
    
    // Command line parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "ds4-translator " << DS4_VERSION << std::endl;
            return 0;
        } else if (arg == "--type" || arg == "-t") {
            if (i + 1 < argc) {
                std::string val = argv[++i];
                if (val == "dualsense") {
                    target_type = TYPE_DUALSENSE;
                    type_explicitly_set = true;
                } else if (val == "ds4") {
                    target_type = TYPE_DS4;
                    type_explicitly_set = true;
                } else if (val == "none") {
                    target_type = TYPE_NONE;
                    type_explicitly_set = true;
                } else if (val == "hidden") {
                    target_type = TYPE_HIDDEN;
                    type_explicitly_set = true;
                } else {
                    std::cerr << "Unknown type: " << val << ". Supported: ds4, dualsense, none, hidden" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Missing value for type option." << std::endl;
                return 1;
            }
        } else if (arg == "--backend" || arg == "-b") {
            if (i + 1 < argc) {
                std::string val = argv[++i];
                if (val == "gadget") {
                    backend_type = BACKEND_GADGET;
                    backend_explicitly_set = true;
                } else if (val == "uhid") {
                    backend_type = BACKEND_UHID;
                    backend_explicitly_set = true;
                } else {
                    std::cerr << "Unknown backend: " << val << ". Supported: uhid, gadget" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Missing value for backend option." << std::endl;
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ds4-translator [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -t, --type <ds4|dualsense|none|hidden>   Target virtual controller type (default: ds4)" << std::endl;
            std::cout << "  -b, --backend <uhid|gadget>   Target backend type (default: auto-detect)" << std::endl;
            std::cout << "  -h, --help                  Show this help message" << std::endl;
            return 0;
        }
    }

    if (!type_explicitly_set) {
        target_type = read_config(target_type);
    }

    if (!backend_explicitly_set) {
        backend_type = BACKEND_UHID;
        std::cout << "Using stable UHID backend by default." << std::endl;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Starting DS4 Translator daemon..." << std::endl;
    std::cout << "Initial Target Emulation: " << controller_type_name(target_type) << std::endl;

    int server_fd = setup_ipc_socket();
    if (server_fd < 0) {
        std::cerr << "Continuing without IPC socket support." << std::endl;
    } else {
        std::cout << "IPC Unix socket created at /run/ds4-translator.sock" << std::endl;
    }

    if (target_type == TYPE_NONE) {
        std::ofstream f("/run/ds4-translator.none");
        f.close();
        system("udevadm trigger");
    } else {
        unlink("/run/ds4-translator.none");
    }

    std::string phy_name = "";
    std::vector<PhysicalNode> hidden_nodes;
    std::string phy_path = "";
    mode_t orig_mode = 0660;

    bool device_open = false;

    // Real USB DS4 hardware streams input reports continuously (~250Hz)
    // even at rest, so physical-passthrough mode inherits that heartbeat
    // for free by forwarding whatever the real controller sends. The
    // standalone virtual controller has no physical device behind it, so
    // without this it stays completely silent between `ds4-ctl tap`/
    // `input` IPC commands — unlike anything a real gamepad would ever
    // do. That silence (easily multiple seconds between manually-typed
    // commands) let a real game's own device-liveness tracking give up
    // on it between button presses, even though each individual report
    // was delivered correctly. Re-emitting the last known synthetic
    // state on a fixed timer keeps the report stream continuous, mirror
    // real hardware's idle behavior.
    auto last_synth_heartbeat = std::chrono::steady_clock::now();
    constexpr auto SYNTH_HEARTBEAT_INTERVAL = std::chrono::milliseconds(8);

    auto last_test_broadcast = std::chrono::steady_clock::now();
    constexpr auto TEST_BROADCAST_INTERVAL = std::chrono::milliseconds(33); // ~30Hz, plenty for a human-readable live view

    // A dropped physical connection (Bluetooth hiccup, brief unplug) used to
    // tear the virtual device down immediately, which is what actually made
    // "emulation stops" intermittent: any game/Wine process that had the
    // device open lost it outright and would only see a *new* device object
    // if/when the controller came back, which some games never notice
    // without a restart. Instead, keep the virtual device alive for a grace
    // window after a drop — the physical side gets fully torn down/restored
    // as before, but the virtual device (and its last-set LED/rumble state
    // in cur_r/cur_g/cur_b) just sits idle. If the controller reconnects
    // within the window, the existing virtual device is reused as-is
    // (LED color included) instead of recreated. Only destroyed for real
    // once the window elapses with no reconnect.
    bool phy_disconnect_pending_destroy = false;
    std::chrono::steady_clock::time_point phy_disconnect_time;
    constexpr auto PHYSICAL_DISCONNECT_GRACE = std::chrono::seconds(300);

    bool type_change_requested = false;
    ControllerType pending_type_change = target_type;

    // `ds4-ctl test` live-monitor state. Subscriber fds get a periodic
    // STATE line (whatever the active source — virtual device or raw
    // physical passthrough — currently looks like) plus an EVENT line
    // whenever an LED/rumble output report actually changes something.
    // Kept local to main() since nothing outside the poll loop touches it.
    std::vector<int> test_subscribers;
    std::deque<std::string> test_event_log; // small ring buffer replayed to new subscribers
    constexpr size_t TEST_EVENT_LOG_MAX = 20;

    struct dualshock4_input_report_common last_phy_common;
    memset(&last_phy_common, 0, sizeof(last_phy_common));
    bool last_phy_valid = false;

    auto test_log_event = [&](const std::string& line) {
        test_event_log.push_back(line);
        if (test_event_log.size() > TEST_EVENT_LOG_MAX) test_event_log.pop_front();
        std::string msg = "EVENT " + line + "\n";
        for (auto it = test_subscribers.begin(); it != test_subscribers.end(); ) {
            if (write(*it, msg.c_str(), msg.size()) < 0) {
                close(*it);
                it = test_subscribers.erase(it);
            } else {
                ++it;
            }
        }
    };

    auto test_broadcast_state = [&]() {
        if (test_subscribers.empty()) return;
        std::string line;
        if (controller_type_emulates(target_type)) {
            bool vdev_exists = (backend_type == BACKEND_GADGET) ? virtual_gadget.device_open : (uhid_fd >= 0);
            if (vdev_exists && last_virtual_valid) {
                char buf[160];
                snprintf(buf, sizeof(buf), "STATE VIRTUAL %u %u %u %u %u %u %02x %02x %02x\n",
                         last_virtual_common.x, last_virtual_common.y, last_virtual_common.rx, last_virtual_common.ry,
                         last_virtual_common.z, last_virtual_common.rz,
                         last_virtual_common.buttons[0], last_virtual_common.buttons[1], last_virtual_common.buttons[2]);
                line = buf;
            } else {
                line = "NOTE No active virtual controller yet (connect the physical controller, or run 'ds4-ctl create-virtual').\n";
            }
        } else if (last_phy_valid) {
            char buf[160];
            snprintf(buf, sizeof(buf), "STATE PHYSICAL %u %u %u %u %u %u %02x %02x %02x\n",
                     last_phy_common.x, last_phy_common.y, last_phy_common.rx, last_phy_common.ry,
                     last_phy_common.z, last_phy_common.rz,
                     last_phy_common.buttons[0], last_phy_common.buttons[1], last_phy_common.buttons[2]);
            line = buf;
        } else {
            line = "NOTE No physical controller connected.\n";
        }
        for (auto it = test_subscribers.begin(); it != test_subscribers.end(); ) {
            if (write(*it, line.c_str(), line.size()) < 0) {
                close(*it);
                it = test_subscribers.erase(it);
            } else {
                ++it;
            }
        }
    };

    while (running) {
        // If physical controller disconnected, scan & setup. Skipped while a
        // standalone virtual controller (created via `ds4-ctl virtual`, no
        // physical hardware involved) is active, so plugging in real
        // hardware later doesn't fight over the same virtual device.
        if (phy_fd < 0 && !standalone_virtual && !ignore_physical) {
            bool bt = false;
            std::string name = find_physical_ds4(bt);
            if (!name.empty()) {
                std::cout << "Found physical DualShock 4: /dev/" << name 
                          << " (Connection: " << (bt ? "Bluetooth" : "USB") << ")" << std::endl;
                
                phy_name = name;
                is_bluetooth = bt;
                phy_path = "/dev/" + phy_name;

                phy_fd = open(phy_path.c_str(), O_RDWR | O_NONBLOCK);
                if (phy_fd < 0) {
                    std::cerr << "Failed to open physical controller: " << strerror(errno) << std::endl;
                    phy_fd = -1;
                    phy_name = "";
                } else {
                    // TYPE_NONE is a fully hands-off passthrough mode: the
                    // physical controller is left completely untouched (no
                    // hidraw/event hiding, no battery hiding) so other apps
                    // see the exact same real device they would without this
                    // daemon running at all. Every other type (TYPE_HIDDEN
                    // included) hides it, same as before.
                    if (target_type != TYPE_NONE) {
                        // Restrict physical hidraw node to 0600 so unprivileged games ignore it
                        struct stat phy_st;
                        if (fstat(phy_fd, &phy_st) == 0) {
                            orig_mode = phy_st.st_mode & 0777;
                        }
                        chmod(phy_path.c_str(), 0600);
                        // A stale ACL from systemd-logind's "uaccess" tag (granted before this
                        // daemon's udev rule could strip it, e.g. if the controller connected
                        // before /run/ds4-translator.sock existed) grants the active user rw
                        // access regardless of the 0600 mode bits above, so it must be cleared
                        // explicitly or unprivileged games can still open the node directly.
                        system(("setfacl -b '" + phy_path + "' 2>/dev/null").c_str());

                        // Grab and hide input events (event and js nodes)
                        std::vector<std::string> event_paths = get_event_nodes(phy_name);
                        for (const auto& ev_path : event_paths) {
                            PhysicalNode node;
                            node.path = ev_path;
                            node.fd = -1;
                            node.orig_mode = 0660;
                            node.is_grabbed = false;

                            struct stat node_st;
                            if (stat(ev_path.c_str(), &node_st) == 0) {
                                node.orig_mode = node_st.st_mode & 0777;
                            }

                            // EVIOCGRAB only blocks input *delivery* to other readers; it does
                            // NOT stop them from opening the node and querying its name/caps
                            // via EVIOCGNAME/EVIOCGBIT. That alone is enough for the browser
                            // Gamepad API / SDL to list the physical pad as a second, dead
                            // controller. Root-only 0600 (mirroring the hidraw fix) actually
                            // keeps other processes from opening it at all.
                            chmod(ev_path.c_str(), 0600);

                            // We only grab event nodes, not js nodes (EVIOCGRAB is only for evdev)
                            if (ev_path.find("event") != std::string::npos) {
                                int ev_fd = open(ev_path.c_str(), O_RDONLY | O_NONBLOCK);
                                if (ev_fd >= 0) {
                                    if (ioctl(ev_fd, EVIOCGRAB, 1) >= 0) {
                                        node.fd = ev_fd;
                                        node.is_grabbed = true;
                                        std::cout << "Successfully grabbed and hid event node: " << ev_path << std::endl;
                                    } else {
                                        std::cerr << "Warning: Failed to grab event node " << ev_path << ": " << strerror(errno) << std::endl;
                                        close(ev_fd);
                                    }
                                }
                            } else {
                                std::cout << "Successfully hid joystick node: " << ev_path << std::endl;
                            }
                            hidden_nodes.push_back(node);
                        }
                    } // target_type != TYPE_NONE

                    bool created = true;
                    bool reused_existing = false;
                    if (target_type == TYPE_NONE) {
                        std::cout << "Emulation disabled. Physical controller left untouched (fully visible to other apps)." << std::endl;
                    } else if (target_type == TYPE_HIDDEN) {
                        std::cout << "Emulation disabled. Hiding physical controller only." << std::endl;
                    } else if (phy_disconnect_pending_destroy) {
                        // Reconnected within the grace window: the virtual
                        // device was never destroyed, so reuse it as-is —
                        // the game/Wine never saw it disappear, and cur_r/
                        // cur_g/cur_b (pushed back to the physical pad
                        // below) still hold whatever LED color was last set.
                        std::cout << "Physical controller reconnected within grace period; reusing existing virtual device." << std::endl;
                        phy_disconnect_pending_destroy = false;
                        reused_existing = true;
                    } else {
                        created = create_virtual_device(target_type);
                    }

                    if (!created) {
                        for (auto& node : hidden_nodes) {
                            if (node.is_grabbed && node.fd >= 0) {
                                ioctl(node.fd, EVIOCGRAB, 0);
                                close(node.fd);
                            }
                            chmod(node.path.c_str(), node.orig_mode);
                        }
                        hidden_nodes.clear();
                        close(phy_fd);
                        phy_fd = -1;
                        chmod(phy_path.c_str(), orig_mode);
                        phy_name = "";
                        usleep(500000); // Sleep 500ms before retrying
                    } else {
                        if (controller_type_emulates(target_type)) {
                            if (!reused_existing) {
                                usleep(100000); // 100ms settling delay for udev properties
                            }
                            // Detached: on some kernels a Bluetooth hidraw write() can
                            // block for several seconds regardless of O_NONBLOCK (the
                            // hidp/L2CAP output path doesn't honor it). Doing this inline
                            // used to stall the main poll loop for that whole window,
                            // starving hid-playstation's probe() of the GET_REPORT
                            // replies it needs and failing DS4 registration outright.
                            // Also re-applies the last known LED color to the physical
                            // pad after a reconnect, since a real DS4 resets its LED
                            // when the connection drops even though our virtual device
                            // (and cur_r/cur_g/cur_b) didn't change.
                            int dup_fd = dup(phy_fd);
                            if (dup_fd >= 0) {
                                std::thread([fd = dup_fd, bt = is_bluetooth, r = cur_r, g = cur_g, b = cur_b]() {
                                    send_physical_output_report(fd, bt, 0, 0, r, g, b);
                                    close(fd);
                                }).detach();
                            }
                        }
                        if (!reused_existing) {
                            device_open = false;
                        }
                    }
                }
            }
        }

        // Build poll FD set
        std::vector<struct pollfd> pfds;
        int phy_poll_idx = -1;
        int uhid_poll_idx = -1;
        int server_poll_idx = -1;

        if (phy_fd >= 0) {
            struct pollfd p;
            p.fd = phy_fd;
            p.events = POLLIN;
            pfds.push_back(p);
            phy_poll_idx = pfds.size() - 1;
        }
        if (backend_type == BACKEND_GADGET) {
            // EP0 events handled by ep0_loop thread — no fd polling needed
        } else {
            if (uhid_fd >= 0) {
                struct pollfd p;
                p.fd = uhid_fd;
                p.events = POLLIN;
                pfds.push_back(p);
                uhid_poll_idx = pfds.size() - 1;
            }
        }
        if (server_fd >= 0) {
            struct pollfd p;
            p.fd = server_fd;
            p.events = POLLIN;
            pfds.push_back(p);
            server_poll_idx = pfds.size() - 1;
        }

        int poll_timeout = standalone_virtual ? 8 : (test_subscribers.empty() ? 1000 : 33);
        int poll_ret = poll(pfds.data(), pfds.size(), poll_timeout);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Poll failed: " << strerror(errno) << std::endl;
            break;
        }

        if (standalone_virtual) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_synth_heartbeat >= SYNTH_HEARTBEAT_INTERVAL) {
                last_synth_heartbeat = now;
                struct dualshock4_input_report_common common = build_synthetic_common(synth_state);
                struct dualshock4_touch_report empty_touch[4];
                memset(empty_touch, 0, sizeof(empty_touch));
                emit_input_report(target_type, common, 0, empty_touch, false);
            }
        }

        if (!test_subscribers.empty()) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_test_broadcast >= TEST_BROADCAST_INTERVAL) {
                last_test_broadcast = now;
                test_broadcast_state();
            }
        }

        // Handle Unix Domain Socket configuration clients
        if (server_poll_idx >= 0 && (pfds[server_poll_idx].revents & POLLIN)) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd >= 0) {
                char rx_buf[256];
                memset(rx_buf, 0, sizeof(rx_buf));
                ssize_t bytes_read = read(client_fd, rx_buf, sizeof(rx_buf) - 1);
                if (bytes_read > 0) {
                    std::string cmd(rx_buf);
                    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
                        cmd.pop_back();
                    }

                    std::string response = "Unknown command";
                    bool keep_open = false; // set by "test": ownership of client_fd moves to test_subscribers
                    if (cmd == "status") {
                        response = "Physical Controller: " + (phy_name.empty() ? "None" : "/dev/" + phy_name) + "\n";
                        response += "Connection Type: " + std::string(phy_fd >= 0 ? (is_bluetooth ? "Bluetooth" : "USB") : "N/A") + "\n";
                        response += "Virtual Emulation: " + std::string(controller_type_name(target_type)) + "\n";
                        response += "Device Open by Host: " + std::string(((backend_type == BACKEND_GADGET) ? virtual_gadget.configured : device_open) ? "Yes" : "No") + "\n";
                        response += "Standalone Virtual (no hardware): " + std::string(standalone_virtual ? "Yes" : "No") + "\n";
                        response += "Physical Auto-Scan Disabled (release-physical): " + std::string(ignore_physical ? "Yes" : "No") + "\n";
                        if (phy_disconnect_pending_destroy) {
                            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                                PHYSICAL_DISCONNECT_GRACE - (std::chrono::steady_clock::now() - phy_disconnect_time));
                            response += "Physical Disconnected, Grace Period: " + std::to_string(remaining.count()) + "s remaining\n";
                        }
                    } else if (cmd == "create-virtual ds4" || cmd == "create-virtual dualsense") {
                        ControllerType want = (cmd == "create-virtual ds4") ? TYPE_DS4 : TYPE_DUALSENSE;
                        if (phy_fd >= 0) {
                            response = "Error: A physical controller is connected and already driving the virtual device.";
                        } else if (standalone_virtual) {
                            response = "Virtual controller already active.";
                        } else if (uhid_fd >= 0 || virtual_gadget.configured) {
                            response = "Error: A virtual device is already active.";
                        } else if (create_virtual_device(want)) {
                            standalone_virtual = true;
                            target_type = want;
                            synth_state = SyntheticState();
                            device_open = false;
                            response = "OK: Standalone virtual controller created.";
                        } else {
                            response = "Error: Failed to create virtual controller (see daemon log).";
                        }
                    } else if (cmd == "destroy-virtual") {
                        if (!standalone_virtual) {
                            response = "Error: No standalone virtual controller is active.";
                        } else {
                            if ((backend_type == BACKEND_GADGET) ? virtual_gadget.configured : device_open) {
                                emit_neutral_report(target_type);
                            }
                            destroy_virtual_device();
                            phy_disconnect_pending_destroy = false;
                            standalone_virtual = false;
                            device_open = false;
                            response = "OK: Standalone virtual controller destroyed.";
                        }
                    } else if (cmd == "release-physical") {
                        if (phy_fd < 0) {
                            ignore_physical = true;
                            response = "OK: No physical controller was connected; auto-scan disabled.";
                        } else {
                            std::cout << "Releasing physical controller by IPC request..." << std::endl;
                            for (auto& node : hidden_nodes) {
                                if (node.is_grabbed && node.fd >= 0) {
                                    ioctl(node.fd, EVIOCGRAB, 0);
                                    close(node.fd);
                                }
                                chmod(node.path.c_str(), node.orig_mode);
                            }
                            hidden_nodes.clear();
                            close(phy_fd);
                            phy_fd = -1;
                            chmod(phy_path.c_str(), orig_mode);
                            phy_name = "";

                            if ((backend_type == BACKEND_GADGET) ? virtual_gadget.configured : device_open) {
                                emit_neutral_report(target_type);
                            }
                            destroy_virtual_device();
                            phy_disconnect_pending_destroy = false;
                            device_open = false;
                            ignore_physical = true;
                            response = "OK: Physical controller released and auto-scan disabled. Use create-virtual, then resume-physical when done.";
                        }
                    } else if (cmd == "resume-physical") {
                        ignore_physical = false;
                        response = "OK: Physical controller auto-scan resumed.";
                    } else if (cmd.rfind("input ", 0) == 0) {
                        if (!standalone_virtual) {
                            response = "Error: No standalone virtual controller is active. Use create-virtual (or ds4-ctl virtual) first.";
                        } else {
                            unsigned int buttons_val, dpad_val, lx_val, ly_val, rx_val, ry_val, l2_val, r2_val;
                            int n = sscanf(cmd.c_str(), "input %x %u %u %u %u %u %u %u",
                                           &buttons_val, &dpad_val, &lx_val, &ly_val, &rx_val, &ry_val, &l2_val, &r2_val);
                            if (n != 8) {
                                response = "Error: malformed input command";
                            } else {
                                synth_state.buttons = (uint16_t)buttons_val;
                                synth_state.dpad = (uint8_t)dpad_val;
                                synth_state.lx = (uint8_t)lx_val;
                                synth_state.ly = (uint8_t)ly_val;
                                synth_state.rx = (uint8_t)rx_val;
                                synth_state.ry = (uint8_t)ry_val;
                                synth_state.l2_analog = (uint8_t)l2_val;
                                synth_state.r2_analog = (uint8_t)r2_val;

                                struct dualshock4_input_report_common common = build_synthetic_common(synth_state);
                                struct dualshock4_touch_report empty_touch[4];
                                memset(empty_touch, 0, sizeof(empty_touch));
                                emit_input_report(target_type, common, 0, empty_touch, false);
                                last_synth_heartbeat = std::chrono::steady_clock::now();
                                response = "OK";
                            }
                        }
                    } else if (cmd == "set-type ds4") {
                        if (target_type != TYPE_DS4) {
                            pending_type_change = TYPE_DS4;
                            type_change_requested = true;
                            write_config(TYPE_DS4);
                            response = "OK: Changing emulation type to DualShock 4...";
                        } else {
                            response = "Already set to DualShock 4";
                        }
                    } else if (cmd == "set-type dualsense") {
                        if (target_type != TYPE_DUALSENSE) {
                            pending_type_change = TYPE_DUALSENSE;
                            type_change_requested = true;
                            write_config(TYPE_DUALSENSE);
                            response = "OK: Changing emulation type to DualSense...";
                        } else {
                            response = "Already set to DualSense";
                        }
                    } else if (cmd == "set-type none") {
                        if (target_type != TYPE_NONE) {
                            pending_type_change = TYPE_NONE;
                            type_change_requested = true;
                            write_config(TYPE_NONE);
                            response = "OK: Changing emulation type to None (translation disabled, physical controller untouched)...";
                        } else {
                            response = "Already set to None";
                        }
                    } else if (cmd == "set-type hidden") {
                        if (target_type != TYPE_HIDDEN) {
                            pending_type_change = TYPE_HIDDEN;
                            type_change_requested = true;
                            write_config(TYPE_HIDDEN);
                            response = "OK: Changing emulation type to Hidden (translation disabled, physical controller hidden)...";
                        } else {
                            response = "Already set to Hidden";
                        }
                    } else if (cmd == "test") {
                        // Turns this connection into a live push stream instead of a
                        // one-shot request/response: client_fd is handed off to
                        // test_subscribers (see the lambdas declared near the top of
                        // main()) rather than closed below, and gets a periodic STATE
                        // line plus an EVENT line on every LED/rumble change until it
                        // disconnects (ds4-ctl's `test` command runs until Ctrl+C).
                        fcntl(client_fd, F_SETFL, O_NONBLOCK);
                        std::string header = "OK: Test monitor active. Type=" + std::string(controller_type_name(target_type));
                        if (controller_type_emulates(target_type)) {
                            header += " Source=Virtual controller\n";
                        } else if (target_type == TYPE_HIDDEN) {
                            header += " Source=Physical controller (hidden)\n"
                                      "CAVEAT Physical controller is hidden -- no other app can reach it to send LED/rumble commands.\n";
                        } else {
                            header += " Source=Physical controller (full passthrough)\n"
                                      "CAVEAT type=none is full passthrough -- LED/rumble commands other apps send directly\n"
                                      "CAVEAT to the physical controller can't be observed here.\n";
                        }
                        keep_open = true;
                        if (write(client_fd, header.c_str(), header.size()) >= 0) {
                            for (const auto& ev : test_event_log) {
                                std::string msg = "EVENT " + ev + "\n";
                                write(client_fd, msg.c_str(), msg.size());
                            }
                            test_subscribers.push_back(client_fd);
                        } else {
                            close(client_fd);
                        }
                    }
                    if (!keep_open) {
                        write(client_fd, response.c_str(), response.size());
                        close(client_fd);
                    }
                }
                else {
                    close(client_fd);
                }
            }
        }

        // Recreate virtual device on dynamic emulation type reload
        if (type_change_requested) {
            if ((backend_type == BACKEND_GADGET) ? virtual_gadget.configured : device_open) {
                emit_neutral_report(target_type); // old type, before it's overwritten below
            }
            bool had_physical = (phy_fd >= 0);
            ControllerType old_type = target_type;
            target_type = pending_type_change;
            type_change_requested = false;

            destroy_virtual_device();
            phy_disconnect_pending_destroy = false;

            // Release physical controller grab and permissions. If a
            // physical controller was connected, deliberately don't
            // recreate the virtual device further down — releasing phy_fd
            // here makes the auto-scan block (top of the loop) rediscover
            // and re-open the same controller on the very next iteration,
            // creating the device bound to the new type through the exact
            // same path a fresh connection uses. Creating it here too
            // (the previous behavior) just leaked a duplicate,
            // immediately-orphaned UHID device once the scan block created
            // its own moments later.
            std::string disconnected_phy_name;
            if (phy_fd >= 0) {
                std::cout << "Releasing physical controller grab..." << std::endl;
                disconnected_phy_name = phy_name;
                for (auto& node : hidden_nodes) {
                    if (node.is_grabbed && node.fd >= 0) {
                        ioctl(node.fd, EVIOCGRAB, 0);
                        close(node.fd);
                    }
                    chmod(node.path.c_str(), node.orig_mode);
                }
                hidden_nodes.clear();
                close(phy_fd);
                phy_fd = -1;
                chmod(phy_path.c_str(), orig_mode);
                phy_name = "";
            }

            if (target_type == TYPE_NONE) {
                std::cout << "Emulation type set to None. Disabling translation and leaving the physical controller untouched." << std::endl;
                standalone_virtual = false;
                std::ofstream f("/run/ds4-translator.none");
                f.close();
                system("udevadm trigger");
            } else if (target_type == TYPE_HIDDEN) {
                std::cout << "Emulation type set to Hidden. Disabling translation but keeping the physical controller hidden." << std::endl;
                standalone_virtual = false;
                unlink("/run/ds4-translator.none");
                system("udevadm trigger");
            } else {
                unlink("/run/ds4-translator.none");
                system("udevadm trigger");

                if (!had_physical && standalone_virtual) {
                    // A standalone virtual controller (no hardware) was
                    // already active — recreate it under the new type so
                    // it doesn't just vanish; nothing else will create it
                    // for us since there's no physical device to trigger
                    // the auto-scan path.
                    if (create_virtual_device(target_type)) {
                        device_open = false;
                    }
                }
                // else: either had_physical (the auto-scan block creates
                // the device once it rediscovers the controller) or
                // nothing was actively emulated before this type change —
                // don't eagerly create a device with nothing to drive it.
            }

            // See rebind_physical_hid_driver()'s doc comment: a permission
            // fixup alone doesn't retroactively affect apps that already
            // had the physical device open, and doesn't notify hotplug-
            // aware apps of a newly-visible device either. Only force this
            // when hidden-ness actually changed (not e.g. ds4<->dualsense,
            // which doesn't affect what other apps can see).
            if (had_physical &&
                controller_type_hides_physical(old_type) != controller_type_hides_physical(target_type)) {
                std::cout << "Forcing physical controller re-enumeration (unbind/rebind) so already-running apps pick up the visibility change..." << std::endl;
                rebind_physical_hid_driver(disconnected_phy_name);
            }
        }

        // Handle physical controller connection dropped
        if (phy_poll_idx >= 0 && (pfds[phy_poll_idx].revents & (POLLERR | POLLHUP))) {
            std::cout << "Physical controller connection dropped." << std::endl;
            for (auto& node : hidden_nodes) {
                if (node.is_grabbed && node.fd >= 0) {
                    ioctl(node.fd, EVIOCGRAB, 0);
                    close(node.fd);
                }
                chmod(node.path.c_str(), node.orig_mode);
            }
            hidden_nodes.clear();
            close(phy_fd);
            phy_fd = -1;
            chmod(phy_path.c_str(), orig_mode);
            phy_name = "";

            // Don't tear the virtual device down yet — clear its input state
            // so nothing looks stuck "held" during the grace window, but
            // leave the device itself (and its LED/rumble state) alive in
            // case the controller comes back. See PHYSICAL_DISCONNECT_GRACE.
            if (controller_type_emulates(target_type) &&
                ((backend_type == BACKEND_GADGET) ? virtual_gadget.configured : device_open)) {
                emit_neutral_report(target_type);
                phy_disconnect_pending_destroy = true;
                phy_disconnect_time = std::chrono::steady_clock::now();
            }
            continue;
        }

        // Grace window for a dropped physical controller expired with no
        // reconnect — actually destroy the virtual device now.
        if (phy_disconnect_pending_destroy &&
            std::chrono::steady_clock::now() - phy_disconnect_time >= PHYSICAL_DISCONNECT_GRACE) {
            std::cout << "Physical controller did not reconnect within "
                      << std::chrono::duration_cast<std::chrono::seconds>(PHYSICAL_DISCONNECT_GRACE).count()
                      << "s; destroying virtual device." << std::endl;
            destroy_virtual_device();
            device_open = false;
            phy_disconnect_pending_destroy = false;
        }

        // Forward physical controller inputs to virtual controller
        if (phy_poll_idx >= 0 && (pfds[phy_poll_idx].revents & POLLIN)) {
            uint8_t in_buf[128];
            ssize_t bytes_read = read(phy_fd, in_buf, sizeof(in_buf));
            if (bytes_read <= 0) {
                if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // ignore
                } else {
                    std::cerr << "Error reading physical controller: " << strerror(errno) << std::endl;
                    // Trigger disconnect manually
                    pfds[phy_poll_idx].revents |= POLLHUP;
                }
            } else {
                // Parsed unconditionally (not just when a virtual device is
                // open) so last_phy_common stays current for `ds4-ctl test`
                // even in "none"/"hidden" mode, where nothing else reads
                // physical reports at all.
                struct dualshock4_input_report_common common;
                bool got_report = false;
                uint8_t num_touch = 0;
                struct dualshock4_touch_report touch_reps[4];
                memset(&common, 0, sizeof(common));
                memset(touch_reps, 0, sizeof(touch_reps));

                if (in_buf[0] == 0x01 && bytes_read >= 64) {
                    struct dualshock4_input_report_usb* usb_in = (struct dualshock4_input_report_usb*)in_buf;
                    common = usb_in->common;
                    num_touch = usb_in->num_touch_reports;
                    for (int i = 0; i < 3 && i < num_touch; ++i) {
                        touch_reps[i] = usb_in->touch_reports[i];
                    }
                    got_report = true;
                } else if (in_buf[0] == 0x11 && bytes_read >= 78) {
                    // Bluetooth input reports carry a CRC32 (seed 0xA1) over the first 74
                    // bytes. hid-playstation validates this in the kernel and drops the
                    // report on mismatch ("DualShock4 input CRC's check failed" in dmesg);
                    // we must do the same, otherwise a corrupted-but-right-length packet
                    // (RF interference, momentary disconnect) gets its garbage bytes
                    // decoded as real stick/button state and forwarded to the game.
                    uint32_t received_crc = (uint32_t)in_buf[74] | ((uint32_t)in_buf[75] << 8) |
                                             ((uint32_t)in_buf[76] << 16) | ((uint32_t)in_buf[77] << 24);
                    uint32_t computed_crc = calculate_crc32(0xA1, in_buf, 74);
                    if (received_crc == computed_crc) {
                        struct dualshock4_input_report_bt* bt_in = (struct dualshock4_input_report_bt*)in_buf;
                        common = bt_in->common;
                        num_touch = bt_in->num_touch_reports;
                        for (int i = 0; i < 4 && i < num_touch; ++i) {
                            touch_reps[i] = bt_in->touch_reports[i];
                        }
                        got_report = true;
                    }
                }

                if (got_report) {
                    last_phy_common = common;
                    last_phy_valid = true;
                    if (((backend_type == BACKEND_GADGET) ? virtual_gadget.configured : device_open) &&
                        ((backend_type == BACKEND_GADGET) || uhid_fd >= 0)) {
                        emit_input_report(target_type, common, num_touch, touch_reps, true);
                    }
                }
            }
        }

        // Handle virtual controller state/queries from Kernel
        if (uhid_poll_idx >= 0 && (pfds[uhid_poll_idx].revents & (POLLERR | POLLHUP))) {
            if (backend_type == BACKEND_GADGET) {
                std::cerr << "Gadget device dropped." << std::endl;
                raw_gadget_close(&virtual_gadget);
            } else {
                std::cerr << "UHID device dropped." << std::endl;
                close(uhid_fd);
                uhid_fd = -1;
            }
            standalone_virtual = false;
            continue;
        }

        if (uhid_poll_idx >= 0 && (pfds[uhid_poll_idx].revents & POLLIN)) {
            if (backend_type == BACKEND_GADGET) {
                // EP0 events are handled by ep0_loop thread — nothing to do here
            } else {
                struct uhid_event kernel_ev;
                ssize_t bytes_read = read(uhid_fd, &kernel_ev, sizeof(kernel_ev));
                if (bytes_read < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "Error reading from uhid: " << strerror(errno) << std::endl;
                        close(uhid_fd);
                        uhid_fd = -1;
                    }
                } else if (bytes_read > 0) {
                switch (kernel_ev.type) {
                    case UHID_START:
                        std::cout << "UHID Device Started by kernel" << std::endl;
                        break;
                    case UHID_STOP:
                        std::cout << "UHID Device Stopped by kernel" << std::endl;
                        break;
                    case UHID_OPEN:
                        std::cout << "UHID Device Opened by host" << std::endl;
                        device_open = true;
                        break;
                    case UHID_CLOSE:
                        std::cout << "UHID Device Closed by host" << std::endl;
                        device_open = false;
                        break;
                    case UHID_GET_REPORT: {
                        struct uhid_event reply_ev;
                        memset(&reply_ev, 0, sizeof(reply_ev));
                        reply_ev.type = UHID_GET_REPORT_REPLY;
                        reply_ev.u.get_report_reply.id = kernel_ev.u.get_report.id;
                        reply_ev.u.get_report_reply.err = 0;
                        
                        uint8_t rnum = kernel_ev.u.get_report.rnum;
                        uint8_t rtype = kernel_ev.u.get_report.rtype;
                        
                        if (rtype == UHID_FEATURE_REPORT) {
                            if (target_type == TYPE_DS4) {
                                if (rnum == 0x02 || rnum == 0x25) {
                                    reply_ev.u.get_report_reply.size = 37;
                                    reply_ev.u.get_report_reply.data[0] = rnum;
                                    // Full DS4 IMU calibration data structure (Gyro biases, Accel biases and scale factors)
                                    static const uint8_t ds4_cal[36] = {
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x04, 0x00, 0xfc, 0x00, 0x04,
                                        0x00, 0xfc, 0x00, 0x04, 0x00, 0xfc,
                                        0x00, 0x04, 0x00, 0x04, 0x00, 0x20,
                                        0x00, 0xe0, 0x00, 0x20, 0x00, 0xe0,
                                        0x00, 0x20, 0x00, 0xe0, 0x00, 0x00
                                    };
                                    memcpy(&reply_ev.u.get_report_reply.data[1], ds4_cal, 36);
                                } else if (rnum == 0x10 || rnum == 0x12) {
                                    reply_ev.u.get_report_reply.size = 16;
                                    reply_ev.u.get_report_reply.data[0] = rnum;
                                    // MAC Address (e8:47:3a:d6:e7:74) & Bluetooth identity info
                                    static const uint8_t mac_info[15] = {
                                        0xe8, 0x47, 0x3a, 0xd6, 0xe7, 0x74,
                                        0x08, 0x25, 0x00, 0x1e, 0x00, 0xee, 0x74, 0xd0, 0xbc
                                    };
                                    memcpy(&reply_ev.u.get_report_reply.data[1], mac_info, 15);
                                } else if (rnum == 0x31 || rnum == 0xa3) {
                                    reply_ev.u.get_report_reply.size = 49;
                                    reply_ev.u.get_report_reply.data[0] = rnum;
                                    // DS4 HW/FW version & extended capabilities
                                    reply_ev.u.get_report_reply.data[35] = 0x38;
                                    reply_ev.u.get_report_reply.data[36] = 0x54;
                                    reply_ev.u.get_report_reply.data[41] = 0x33;
                                    reply_ev.u.get_report_reply.data[42] = 0x20;
                                }
                            } else { // DualSense
                                if (rnum == 0x05) {
                                    reply_ev.u.get_report_reply.size = 41;
                                    uint8_t cal_data[41] = {
                                        0x05,
                                        0xff, 0xfc, 0xff, 0xfe, 0xff, 0x83, 0x22, 0x78,
                                        0xdd, 0x92, 0x22, 0x5f, 0xdd, 0x95, 0x22, 0x6d,
                                        0xdd, 0x1c, 0x02, 0x1c, 0x02, 0xf2, 0x1f, 0xed,
                                        0xdf, 0xe3, 0x20, 0xda, 0xe0, 0xee, 0x1f, 0xdf,
                                        0xdf, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                                    };
                                    memcpy(reply_ev.u.get_report_reply.data, cal_data, 41);
                                } else if (rnum == 0x20) {
                                    reply_ev.u.get_report_reply.size = 64;
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
                                    memcpy(reply_ev.u.get_report_reply.data, fw_data, 64);
                                } else if (rnum == 0x09) {
                                    reply_ev.u.get_report_reply.size = 20;
                                    uint8_t pairing_data[20] = {
                                        0x09,
                                        0xe8, 0x47, 0x3a, 0xd6, 0xe7, 0x74,
                                        0x08, 0x25, 0x00, 0x1e, 0x00, 0xee, 0x74, 0xd0, 0xbc,
                                        0x00, 0x00, 0x00, 0x00
                                    };
                                    memcpy(reply_ev.u.get_report_reply.data, pairing_data, 20);
                                }
                            }
                        }
                        uhid_write(uhid_fd, reply_ev);
                        break;
                    }
                    case UHID_SET_REPORT: {
                        struct uhid_event reply_ev;
                        memset(&reply_ev, 0, sizeof(reply_ev));
                        reply_ev.type = UHID_SET_REPORT_REPLY;
                        reply_ev.u.set_report_reply.id = kernel_ev.u.set_report.id;
                        reply_ev.u.set_report_reply.err = 0;
                        uhid_write(uhid_fd, reply_ev);
                        break;
                    }
                    case UHID_OUTPUT: {
                        uint16_t size = kernel_ev.u.output.size;
                        const uint8_t* data = kernel_ev.u.output.data;
                        
                        uint8_t motor_left = cur_motor_left;
                        uint8_t motor_right = cur_motor_right;
                        uint8_t r = cur_r, g = cur_g, b = cur_b;
                        bool update = false;
                        
                        if (target_type == TYPE_DS4) {
                            if (data[0] == 0x05 && size >= 9) {
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
                            } else if (data[0] == 0x11 && size >= 11) { // BT format output report
                                uint8_t flags = data[3];
                                if (flags & 0x01) {
                                    motor_right = data[6];
                                    motor_left = data[7];
                                    update = true;
                                }
                                if (flags & 0x02) {
                                    r = data[8];
                                    g = data[9];
                                    b = data[10];
                                    update = true;
                                }
                            }
                        } else { // DualSense
                            if (data[0] == 0x02 && size >= 48) {
                                uint8_t vf0 = data[1];
                                uint8_t vf1 = data[2];
                                if (vf0 & 0x01) {
                                    motor_right = data[3];
                                    motor_left = data[4];
                                    // If haptics/sound-select (vf0 & 0x02) is active,
                                    // don't run both motors at the same time to prevent
                                    // the big motor from drowning out the light motor.
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

                        if (update) {
                            if (motor_left != cur_motor_left || motor_right != cur_motor_right || r != cur_r || g != cur_g || b != cur_b) {
                                if (r != cur_r || g != cur_g || b != cur_b) {
                                    char buf[64];
                                    snprintf(buf, sizeof(buf), "LED r=%u g=%u b=%u", r, g, b);
                                    test_log_event(buf);
                                }
                                if (motor_left != cur_motor_left || motor_right != cur_motor_right) {
                                    char buf[64];
                                    snprintf(buf, sizeof(buf), "RUMBLE left=%u right=%u", motor_left, motor_right);
                                    test_log_event(buf);
                                }
                                cur_motor_left = motor_left;
                                cur_motor_right = motor_right;
                                cur_r = r;
                                cur_g = g;
                                cur_b = b;
                                if (phy_fd >= 0) {
                                    send_physical_output_report(phy_fd, is_bluetooth, cur_motor_left, cur_motor_right, cur_r, cur_g, cur_b);
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
    }

    // Clean up Unix socket
    for (int fd : test_subscribers) {
        close(fd);
    }
    if (server_fd >= 0) {
        close(server_fd);
        unlink("/run/ds4-translator.sock");
    }

    // Clean up physical controller if active
    if (phy_fd >= 0) {
        std::cout << "Releasing physical controller grab..." << std::endl;
        for (auto& node : hidden_nodes) {
            if (node.is_grabbed && node.fd >= 0) {
                ioctl(node.fd, EVIOCGRAB, 0);
                close(node.fd);
            }
            chmod(node.path.c_str(), node.orig_mode);
        }
        close(phy_fd);
        chmod(phy_path.c_str(), orig_mode);
    }

    // Destroy virtual controller
    if ((backend_type == BACKEND_GADGET) ? virtual_gadget.configured : device_open) {
        emit_neutral_report(target_type);
    }
    destroy_virtual_device();

    std::cout << "DS4 Translator daemon stopped." << std::endl;
    return 0;
}
