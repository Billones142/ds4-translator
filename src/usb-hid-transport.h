#pragma once

// Raw USB interrupt-transfer transport for the default full-unbind hide
// method (see hid-unbind-detect.h). Once the kernel HID driver has
// been unbound via sysfs, no /dev/hidraw node exists for anyone --
// including this daemon -- so reading/writing reports has to bypass the
// kernel driver entirely via libusb instead.
//
// To keep the rest of main.cpp (report parsing at read() time, output
// report building, the poll()-driven main loop, disconnect-via-POLLHUP
// handling) completely unaware of which transport is active, this hands
// back an ordinary fd from a blocking AF_UNIX SOCK_DGRAM socketpair: a
// dedicated background thread shuttles raw HID reports between that
// socket and libusb interrupt transfers, one HID report per
// read()/write() (SOCK_DGRAM preserves message boundaries, matching
// hidraw's per-read()-one-report framing). The fd returned here behaves
// like a hidraw fd in every way that matters to the caller: pollable,
// POLLIN when a report is available, write()-able with output reports,
// and closes/EOFs like a real disconnect if the USB device goes away.

#include <cstdint>
#include <string>

// Unbinds-then-opens is the caller's job (see hid_id_unbind() in
// hid-unbind-detect.h) -- this only takes over from there: finds the
// libusb device matching vid/pid and claims its HID interface. hid_id is
// kept only so a failed/closed transport can fall back to re-binding the
// exact same sysfs device via write_hid_driver_sysfs("bind", hid_id),
// never leaving the controller ownerless.
//
// Deliberately does NOT start pulling reports yet -- see
// usb_hid_transport_start(). Returns a pollable fd on success (not yet
// producing data until started), or -1 on failure -- callers should
// treat -1 the same as open_and_hide_physical() failing: fall back to
// the legacy hide method, and this function has already best-effort
// re-bound the kernel driver before returning -1 so the controller
// isn't left stranded.
int usb_hid_transport_open(const std::string& hid_id, uint16_t vid, uint16_t pid);

// Starts the background thread that actually shuttles reports between
// the claimed USB interface and the fd usb_hid_transport_open() returned.
// Split out from open() because bringing up a same-VID/PID FunctionFS
// virtual gadget (see functionfs-backend.c) concurrently with active
// interrupt transfers on the real device has been observed to make the
// real device's USB port fail entirely (kernel logs "device descriptor
// read/64, error -71", port power-cycle, "unable to enumerate USB
// device" -- confirmed via an isolated libusb-only reproduction that the
// unbind+claim+transfer sequence is fine *by itself*; the failure only
// appears alongside FunctionFS gadget bring-up). Callers should claim
// the interface early (open) but defer traffic (start) until after the
// virtual device is fully up, so the two USB-core operations never
// overlap in time. No-op if already started or if fd is unknown.
void usb_hid_transport_start(int fd);

// Tears down the transport for an fd previously returned by
// usb_hid_transport_open(): stops the shuttle thread, releases the
// libusb interface, attempts to re-attach the kernel driver (falling
// back to an explicit sysfs bind write if that fails), and closes both
// the fd and its internal socket peer. Safe to call with any fd -- if it
// wasn't opened by usb_hid_transport_open(), this just closes it like a
// plain fd would.
void usb_hid_transport_close(int fd);
