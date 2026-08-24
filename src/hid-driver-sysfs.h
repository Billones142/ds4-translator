#ifndef HID_DRIVER_SYSFS_H
#define HID_DRIVER_SYSFS_H

#include <string>

// Best-effort: writes a hid_device sysfs id (e.g. "0003:054C:05C4.0042")
// to the playstation driver's bind/unbind file. Failures are logged but
// otherwise non-fatal. Defined in main.cpp (shared with
// hid-unbind-detect.cpp / usb-hid-transport.cpp so the unbind hide method
// reuses the exact same sysfs-write primitive the legacy hot-swap path
// already relies on, instead of duplicating it).
void write_hid_driver_sysfs(const char* action, const std::string& hid_id);

// Same, for an arbitrary driver under /sys/bus/hid/drivers/. Used by the
// unlink hide method to move a Bluetooth controller onto hid-generic; the
// wrapper above is this with driver = "playstation".
void write_hid_driver_sysfs_for(const char* driver, const char* action, const std::string& hid_id);

#endif // HID_DRIVER_SYSFS_H
