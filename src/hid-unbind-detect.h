#pragma once

// HID-bus-level detection for the default full-unbind hide method (see
// TODO.md's last item, and src/usb-hid-transport.h for the transport
// side; the alternative chmod/setfacl/EVIOCGRAB method lives in
// open_and_hide_physical() in main.cpp, selectable via `ds4-ctl
// set-hide-method legacy`). Generalizes the sysfs-scan idea
// find_usb_ds4_hid_id() in main.cpp already uses for the
// Bluetooth-blocks-USB corner case into the primary detection path for
// this method: watch /sys/bus/hid/devices "add" events at the HID-bus
// level, before hid-playstation ever creates a hidraw node, so the
// controller can be unbound before any other app can see it -- rather
// than existing-but-permission-blocked like the legacy method.
//
// Both bus 0003 (USB) and bus 0005 (Bluetooth) hid_ids are matched here.
// USB gets a real replacement transport (see usb-hid-transport.h): unbind,
// then claim the raw interface via libusb so this daemon can still read
// it. Bluetooth does not -- on this system's BlueZ config (UserspaceHID
// default true), bluetoothd owns the actual L2CAP session itself,
// independently of which kernel driver (if any) is bound to the resulting
// hid_device; taking that session over requires bluetoothd to fully
// disconnect the device's ACL link (confirmed live -- there is no partial/
// profile-only teardown that leaves the link up), which is a materially
// worse trade than this method's USB side. Per explicit testing/decision,
// Bluetooth only gets the sysfs unbind: the controller stays connected and
// is completely invisible to every other app, but this daemon can't read
// it either while it's in that state (see open_and_hide_physical_unbind()
// in main.cpp).

#include <string>

// Opens a NETLINK_KOBJECT_UEVENT socket bound to the kernel's raw uevent
// multicast group (group 1 -- the same "kobject_uevent" broadcast udevd
// itself listens to, but consumed directly here so this works even if
// udev decides not to react in time). Requires root (already assumed --
// the daemon already writes to sysfs bind/unbind files). Returns a
// pollable, non-blocking fd suitable for the main poll() set, or -1 on
// failure (caller should fall back to the legacy hide method entirely,
// since without this there's no way to catch a hidraw-less device before
// another app could theoretically race it).
int hid_uevent_monitor_open();

// Reads and parses exactly one pending uevent datagram from a monitor fd
// previously returned by hid_uevent_monitor_open(). Returns true and
// fills out_hid_id (e.g. "0003:054C:05C4.0042") only for an ACTION=add
// SUBSYSTEM=hid event whose HID_ID matches a physical (non-virtual)
// DualShock4/DualSense over USB or Bluetooth. Any other uevent (remove,
// change, other subsystem, unmatched HID_ID) returns false -- the
// datagram is always consumed either way, so a non-matching event
// doesn't spin poll(). Safe to call again immediately if more than one
// event is queued; the next poll() wakeup (level-triggered) will pick up
// anything left unread this pass.
bool hid_uevent_monitor_read(int fd, std::string& out_hid_id);

// Synchronous scan of /sys/bus/hid/devices for a physical USB or
// Bluetooth DS4/DualSense hid_device that's currently bound to a driver
// (i.e. still needs unbinding -- an already-unbound entry is either
// mid-USB-transport already or a Bluetooth connection this method has
// deliberately left hidden-and-unreadable, neither of which this should
// re-surface as "newly found"). Also used as the startup fallback for
// devices whose "add" event already fired before
// hid_uevent_monitor_open() was called. Excludes this daemon's own
// virtual device the same way find_usb_ds4_hid_id() in main.cpp does.
// Returns "" if none found.
std::string hid_bus_scan_existing();

// Unbinds a hid_device (by its sysfs id) from its currently-bound kernel
// driver via sysfs, so no hidraw/input node is ever created for it. Thin
// wrapper around write_hid_driver_sysfs() (hid-driver-sysfs.h) -- kept as
// a separate name so call sites read as "unbind in order to hide" rather
// than the generic sysfs primitive shared with the legacy hot-swap path.
bool hid_id_unbind(const std::string& hid_id);

// Parses a sysfs hid_id of the form "BBBB:VVVV:PPPP.NNNN"
// (bus:vendor:product.instance, all hex) into numeric bus/vendor/product.
// Used to get a plain vid/pid pair for usb_hid_transport_open(), which
// matches against libusb's device list rather than sysfs paths. Returns
// false if the string doesn't parse.
bool parse_hid_id(const std::string& hid_id, unsigned& bus, unsigned& vendor, unsigned& product);
