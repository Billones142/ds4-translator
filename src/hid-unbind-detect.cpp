#include "hid-unbind-detect.h"
#include "hid-driver-sysfs.h"

#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <unistd.h>

namespace fs = std::filesystem;

int hid_uevent_monitor_open() {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        std::cerr << "hid-unbind: failed to open uevent netlink socket: " << strerror(errno) << std::endl;
        return -1;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0; // let the kernel assign a unique id
    addr.nl_groups = 1; // group 1 == raw kernel "kobject_uevent" broadcast

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "hid-unbind: failed to bind uevent netlink socket: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    return fd;
}

// Matches the same physical-controller identity find_usb_ds4_hid_id() in
// main.cpp checks -- VID 054c, PID 05c4 (DS4 v1) or 09cc (DS4 v2); the
// daemon only ever scans for a physical DualShock 4, never a physical
// DualSense (0ce6 is only used when *creating* a virtual DualSense to
// translate to, see main.cpp's create2.product) -- but against the
// uevent's HID_ID= environment field, which the kernel formats with 8
// hex digits per component (e.g. "0003:0000054C:000005C4") rather than
// the 4-digit form used in sysfs directory names. Bus 0003 (USB) and
// 0005 (Bluetooth) both match -- see hid-unbind-detect.h for why they're
// handled so differently once matched.
static bool hid_id_env_matches_ds4(const std::string& val) {
    if (val.rfind("0003:", 0) != 0 && val.rfind("0005:", 0) != 0) return false;
    if (val.find("0000054C") == std::string::npos && val.find("0000054c") == std::string::npos) return false;
    return val.find("000005C4") != std::string::npos || val.find("000005c4") != std::string::npos ||
           val.find("000009CC") != std::string::npos || val.find("000009cc") != std::string::npos;
}

bool hid_uevent_monitor_read(int fd, std::string& out_hid_id) {
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';

    // Kernel uevent wire format: "ACTION@DEVPATH\0KEY=VALUE\0KEY=VALUE\0...\0"
    std::string action, devpath, hid_id_env;
    size_t pos = 0;
    bool first = true;
    while (pos < (size_t)n) {
        std::string token(buf + pos);
        pos += token.size() + 1;
        if (first) {
            first = false;
            continue; // "ACTION@DEVPATH" header line, redundant with ACTION=/DEVPATH= below
        }
        if (token.rfind("ACTION=", 0) == 0) {
            action = token.substr(7);
        } else if (token.rfind("SUBSYSTEM=", 0) == 0) {
            if (token.substr(10) != "hid") return false;
        } else if (token.rfind("DEVPATH=", 0) == 0) {
            devpath = token.substr(8);
        } else if (token.rfind("HID_ID=", 0) == 0) {
            hid_id_env = token.substr(7);
        }
        if (token.empty()) break;
    }

    if (action != "add" || devpath.empty() || hid_id_env.empty()) return false;
    if (!hid_id_env_matches_ds4(hid_id_env)) return false;

    // Exclude this daemon's own virtual device -- see hid_bus_scan_existing()
    // and find_usb_ds4_hid_id()/is_own_virtual_hidraw() in main.cpp for the
    // full rationale. This matters *far* more here than for the directory
    // scan: creating the FunctionFS or uhid virtual gadget (same VID/PID by
    // design) fires its own genuine "add" uevent, and with no phy_fd yet
    // claimed for a Bluetooth-unbind-hidden connection (which never gets one
    // at all -- see open_and_hide_physical_unbind() in main.cpp), the
    // top-of-loop auto-scan runs every single iteration and would grab that
    // self-uevent immediately, unbind and libusb-claim the daemon's own
    // virtual USB gadget out from under the FunctionFS backend actively
    // serving it, and wedge the daemon hard enough to need SIGKILL --
    // confirmed live. Bus 0003 is forced for both backends (functionfs is
    // real bus 0003 hardware under dummy_hcd; uhid backend forces it in
    // create2.bus), so unlike hid_id_env_matches_ds4()'s bus 0003-or-0005
    // gate, only 0003 needs the uhid-devpath exclusion -- a bus 0005 hid_id
    // under /devices/virtual/misc/uhid/* is a real Bluetooth controller
    // bridged by bluetoothd's own userspace HID handling, not us.
    bool is_bus_0003 = hid_id_env.rfind("0003:", 0) == 0;
    if (devpath.find("dummy_hcd") != std::string::npos) return false;
    if (is_bus_0003 && devpath.find("/devices/virtual/misc/uhid/") != std::string::npos) return false;

    size_t slash = devpath.find_last_of('/');
    out_hid_id = (slash == std::string::npos) ? devpath : devpath.substr(slash + 1);
    return !out_hid_id.empty();
}

std::string hid_bus_scan_existing() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/bus/hid/devices", ec)) {
        std::string id = entry.path().filename().string();
        if (id.rfind("0003:054C:05C4.", 0) != 0 &&
            id.rfind("0003:054C:09CC.", 0) != 0 &&
            id.rfind("0005:054C:05C4.", 0) != 0 &&
            id.rfind("0005:054C:09CC.", 0) != 0) {
            continue;
        }
        fs::path resolved = fs::canonical(entry.path(), ec);
        if (ec) continue;
        // Exclude this daemon's own virtual device -- see
        // find_usb_ds4_hid_id()'s comment in main.cpp for the full
        // rationale (functionfs backend lives under dummy_hcd, uhid
        // backend registers under /devices/virtual/misc/uhid/* with bus
        // forced to BUS_USB). Bus 0005 is never excluded on the uhid check
        // -- a *real* Bluetooth controller also legitimately lives under
        // /devices/virtual/misc/uhid/* whenever bluetoothd bridges it via
        // its own userspace uhid (UserspaceHID=true, the BlueZ default --
        // confirmed live on this system), so that devpath alone can't
        // distinguish "our virtual device" from "the real Bluetooth
        // controller" the way it can for bus 0003. Mirrors
        // is_own_virtual_hidraw()'s identical bus-aware check in main.cpp.
        std::string devpath = resolved.string();
        if (devpath.find("dummy_hcd") != std::string::npos) continue;
        if (id.rfind("0003:", 0) == 0 && devpath.find("/devices/virtual/misc/uhid/") != std::string::npos) continue;
        // Already unbound -- either a USB device already claimed via
        // libusb (tracked by phy_fd being valid, so this scan wouldn't
        // even run for it) or a Bluetooth connection this method has
        // deliberately left hidden-and-unreadable. Either way, nothing
        // to do; re-surfacing it as "newly found" would either be a
        // no-op (USB) or spin forever re-logging the same Bluetooth
        // device every poll iteration (see open_and_hide_physical_unbind()
        // in main.cpp for the Bluetooth side of this).
        if (!fs::exists(entry.path() / "driver", ec)) continue;
        return id;
    }
    return "";
}

bool hid_id_unbind(const std::string& hid_id) {
    write_hid_driver_sysfs("unbind", hid_id);
    // write_hid_driver_sysfs() only logs failures, so confirm success the
    // same way the kernel would show it: the hid_device's "driver"
    // symlink disappears once nothing is bound to it.
    std::error_code ec;
    bool still_bound = fs::exists("/sys/bus/hid/devices/" + hid_id + "/driver", ec);
    return !still_bound;
}

bool parse_hid_id(const std::string& hid_id, unsigned& bus, unsigned& vendor, unsigned& product) {
    size_t c1 = hid_id.find(':');
    if (c1 == std::string::npos) return false;
    size_t c2 = hid_id.find(':', c1 + 1);
    if (c2 == std::string::npos) return false;
    size_t dot = hid_id.find('.', c2 + 1);
    std::string bus_s = hid_id.substr(0, c1);
    std::string vendor_s = hid_id.substr(c1 + 1, c2 - c1 - 1);
    std::string product_s = (dot == std::string::npos) ? hid_id.substr(c2 + 1) : hid_id.substr(c2 + 1, dot - c2 - 1);
    if (bus_s.empty() || vendor_s.empty() || product_s.empty()) return false;

    errno = 0;
    char* end = nullptr;
    unsigned long b = strtoul(bus_s.c_str(), &end, 16);
    if (*end != '\0' || errno == ERANGE) return false;
    unsigned long v = strtoul(vendor_s.c_str(), &end, 16);
    if (*end != '\0' || errno == ERANGE) return false;
    unsigned long p = strtoul(product_s.c_str(), &end, 16);
    if (*end != '\0' || errno == ERANGE) return false;

    bus = (unsigned)b;
    vendor = (unsigned)v;
    product = (unsigned)p;
    return true;
}
