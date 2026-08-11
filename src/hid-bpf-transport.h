#ifndef HID_BPF_TRANSPORT_H
#define HID_BPF_TRANSPORT_H

// Bluetooth replacement transport for the default full-unbind hide
// method (see hid-unbind-detect.h), filling the gap
// open_and_hide_physical_unbind()'s bus 0x0005 branch used to leave
// open: unlike USB, Bluetooth can't be handed off to libusb (bluetoothd
// owns the real L2CAP session regardless of kernel driver binding), so
// there was previously no way to both hide a Bluetooth DS4 completely
// *and* keep reading it.
//
// Uses HID-BPF instead of sysfs unbind: the kernel driver stays bound
// (hidraw/evdev nodes for the physical controller still exist) and a
// struct_ops program (src/hid-bpf/hid-bpf-transport.bpf.c) intercepts
// every inbound report before the HID core dispatches it anywhere else,
// mirrors it into a ring buffer for this daemon, and drops the original
// -- confirmed live (2026-08-10) that this leaves hidraw/evdev
// completely silent for every other consumer while this daemon still
// gets every report, with zero permission-window race and no unbind at
// all. Outbound (LED/rumble) is a separate, untouched path: this
// transport forwards this daemon's own writes straight to the
// still-live hidraw node.
//
// Loaded/attached via the `udev-hid-bpf` CLI (a system package
// dependency, same tier as libusb-1.0 for the USB side -- see Makefile)
// rather than this daemon doing its own libbpf struct_ops attach/hid_id
// plumbing, since only the CLI's device-matching/attach path has
// actually been verified against real hardware.

#include <string>

// Loads and attaches the HID-BPF swallow-and-mirror program to hid_id
// (e.g. "0005:054C:05C4.0008", from hid_bus_scan_existing()/the uevent
// monitor -- see hid-unbind-detect.h), opens the still-live hidraw node
// for LED/rumble writes, and returns a pollable fd that behaves like a
// hidraw fd to the rest of main.cpp: POLLIN when a report is available,
// write()-able with output reports, EOFs like a disconnect if the
// controller goes away. Returns -1 on any failure (udev-hid-bpf missing,
// BPF load/attach failure, no hidraw node found, ...) -- callers should
// treat this exactly like open_and_hide_physical_unbind()'s USB branch
// failing: fall back to the legacy hide method for this connection
// attempt, since nothing about the physical controller's binding was
// touched on failure (no rebind needed, unlike the sysfs-unbind USB
// path).
//
// On success, also fills out_hidraw_name with the bare hidraw device
// name (e.g. "hidraw6") -- the kernel driver stays bound under this
// transport (unlike USB unbind), so hid-playstation's evdev/joystick
// nodes for the physical controller still exist and still enumerate
// (frozen, no data -- hid_device_event swallows every report before
// hid-playstation ever sees one to translate). That's enough to stop
// double *input*, but browsers/SDL query evdev node capabilities
// directly to list connected gamepads regardless of whether any data
// ever arrives (confirmed live, 2026-08-10) -- so the caller still
// needs get_event_nodes(out_hidraw_name) and the
// same EVIOCGRAB+chmod treatment open_and_hide_physical() already gives
// the legacy method's event/js siblings, to hide the physical
// controller's mere *presence* too.
int hid_bpf_transport_open(const std::string& hid_id, std::string& out_hidraw_name);

// Tears down a transport previously returned by hid_bpf_transport_open():
// stops the shuttle thread, detaches the BPF program via `udev-hid-bpf
// remove`, closes the hidraw node and both fds. Safe to call with any fd
// -- if it wasn't opened by hid_bpf_transport_open(), this just closes it
// like a plain fd would.
void hid_bpf_transport_close(int fd);

#endif // HID_BPF_TRANSPORT_H
