#pragma once

#include <string>

// Best-effort: writes a hid_device sysfs id (e.g. "0003:054C:05C4.0042")
// to the playstation driver's bind/unbind file. Failures are logged but
// otherwise non-fatal. Defined in main.cpp (shared with
// hid-unbind-detect.cpp / usb-hid-transport.cpp so the unbind hide method
// reuses the exact same sysfs-write primitive the legacy hot-swap path
// already relies on, instead of duplicating it).
void write_hid_driver_sysfs(const char* action, const std::string& hid_id);
