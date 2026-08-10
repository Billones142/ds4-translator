// SPDX-License-Identifier: GPL-2.0-only
//
// Kernel-side half of the HID-BPF Bluetooth transport (see
// hid-bpf-transport.h/.cpp). Loaded/attached via `udev-hid-bpf add`
// (hid-bpf-transport.cpp shells out to it -- see run_udev_hid_bpf()) so
// this daemon doesn't need to reimplement HID_BPF_CONFIG device matching
// or struct_ops hid_id plumbing itself.
//
// hid_device_event copies every raw input report into a ring buffer for
// this daemon, then returns a negative value so the HID core drops the
// original before hidraw/evdev/LED clients ever see it -- confirmed live
// (2026-08-10, tools/hid-bpf-probe/) that this makes every other
// consumer of the hid_device fully blind, with zero permission-window
// race, while this daemon still gets every report via the ring buffer.
//
// Deliberately does NOT implement hid_rdesc_fixup. An earlier version of
// this same swallow logic also zeroed the report descriptor to suppress
// evdev/js node creation too, and that crashed the kernel on real
// hardware (NULL deref in dispatch_hid_bpf_device_event, called from
// bluetoothd's uhid write path) the moment the next report arrived --
// see tools/hid-bpf-probe/README.md for the full writeup. hidraw/evdev
// nodes for the physical controller stay visible under this transport
// (just carrying no data), which is why hid_bpf_transport_open() still
// needs the hidraw node's own permissions to keep other apps out of it.
//
// Outbound (LED/rumble) is untouched by this program -- hid_device_event
// is inbound-only. Writes to the still-live hidraw node reach the
// physical controller exactly as before; hid-bpf-transport.cpp forwards
// this daemon's own output-report writes there directly.

#include "vmlinux.h"
#include "hid_bpf.h"
#include "hid_bpf_helpers.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

// Sony DualShock 4 (v1 and v2), Bluetooth only -- the USB side of the
// unbind hide method already has a real transport via libusb (see
// usb-hid-transport.h) and doesn't need this. Matches
// hid_id_env_matches_ds4() in hid-unbind-detect.cpp's VID/PID set.
#define VID_SONY 0x054C
#define PID_DS4_V1 0x05C4
#define PID_DS4_V2 0x09CC

HID_BPF_CONFIG(
	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_GENERIC, VID_SONY, PID_DS4_V1),
	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_GENERIC, VID_SONY, PID_DS4_V2)
);

// DS4's largest Bluetooth input report is 78 bytes -- matches
// usb-hid-transport.cpp's kMaxReportSize rationale.
#define MAX_REPORT_LEN 78

// No explicit pinning attribute -- udev-hid-bpf's own loader pins every
// map for this program under /sys/fs/bpf/hid/<hid_id_with_underscores>/
// <program>/ds4_reports; hid-bpf-transport.cpp's find_pinned_ringbuf()
// locates it by scanning for this map's name rather than predicting the
// full path (confirmed via testing that setting LIBBPF_PIN_BY_NAME here
// collides with the loader's own pinning: "map already has pin path ...
// different from ...").
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 64 * 4096);
} ds4_reports SEC(".maps");

SEC(HID_BPF_DEVICE_EVENT)
int BPF_PROG(ds4_bt_swallow_event, struct hid_bpf_ctx *hid_ctx)
{
	__u8 *data = hid_bpf_get_data(hid_ctx, 0, MAX_REPORT_LEN);
	__u8 *slot;

	if (!data)
		return 0; // couldn't read it -- nothing to mirror, let it
		          // through rather than silently blackholing.

	slot = bpf_ringbuf_reserve(&ds4_reports, MAX_REPORT_LEN, 0);
	if (slot) {
		__builtin_memcpy(slot, data, MAX_REPORT_LEN);
		bpf_ringbuf_submit(slot, 0);
	}

	// Negative == HID core drops this event before hidraw/input/LED
	// clients ever see it. The ring buffer copy above already has it.
	return -1;
}

HID_BPF_OPS(ds4_bt_transport) = {
	.hid_device_event = (void *)ds4_bt_swallow_event,
};

char _license[] SEC("license") = "GPL";
