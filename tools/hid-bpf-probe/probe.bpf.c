// SPDX-License-Identifier: GPL-2.0-only
//
// Phase 1 feasibility spike for the Bluetooth unbind-hide fix (see
// /home/stefano/.claude/plans/search-on-the-internet-whimsical-possum.md).
// Not wired into the daemon. Attaches to a DualShock 4 hid_device
// (USB or Bluetooth, hid-playstation-bound) and:
//   - hid_rdesc_fixup: hands back an empty report descriptor, so the
//     bound driver's hid-input parse finds no application collection
//     and never creates /dev/input/event*/js* nodes for this device.
//   - hid_device_event: copies every raw input report into a ring
//     buffer for userspace, then returns a negative value so the HID
//     core drops the original -- per the HID-BPF docs, a negative
//     return here means "hidraw/input/LED clients will not see this
//     event" -- i.e. every OTHER consumer of this hid_device sees
//     nothing, while this ring buffer still gets every report.
// hid_hw_request/hid_hw_output_report are deliberately left
// unimplemented: outbound (LED/rumble) reports are a separate,
// unaffected path and still need to reach the controller through the
// normal (still-bound) driver -- see the plan's "open risks" section.
//
// Loaded via `udev-hid-bpf` against a specific hid_id/VID:PID rather
// than a hand-rolled libusb-style loader in this spike -- see README.md
// in this directory for exact build/load/test steps.

#include "vmlinux.h"
#include "hid_bpf.h"
#include "hid_bpf_helpers.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

// Sony DualShock 4 (v1 and v2). Real Bluetooth PID is 0x05C4/0x09CC same
// as USB; bus is what differs -- see hid_id_env_matches_ds4() in
// src/hid-unbind-detect.cpp for the matching logic this mirrors.
#define VID_SONY 0x054C
#define PID_DS4_V1 0x05C4
#define PID_DS4_V2 0x09CC

HID_BPF_CONFIG(
	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_GENERIC, VID_SONY, PID_DS4_V1),
	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_GENERIC, VID_SONY, PID_DS4_V2)
);

// Max DS4 Bluetooth input report is 78 bytes (matches usb-hid-transport.cpp's
// kMaxReportSize rationale) -- sized generously per-slot since ringbuf
// space is reserved per report, not preallocated per-slot.
#define MAX_REPORT_LEN 78

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 64 * 4096);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} ds4_reports SEC(".maps");

SEC(HID_BPF_RDESC_FIXUP)
int BPF_PROG(ds4_probe_fix_rdesc, struct hid_bpf_ctx *hctx)
{
	__u8 *data = hid_bpf_get_data(hctx, 0, HID_MAX_DESCRIPTOR_SIZE);

	if (!data)
		return 0;

	// Truncate to a single empty top-level collection so hid-input's
	// descriptor parse produces no input application (no evdev/js
	// nodes) -- hidraw registration itself isn't gated on descriptor
	// content, so the (now permanently silent, per the event hook
	// below) hidraw node may still exist, which is fine.
	__builtin_memset(data, 0, HID_MAX_DESCRIPTOR_SIZE);
	return 1; // new descriptor length: an all-zero single byte is not
	          // parseable as any collection.
}

SEC(HID_BPF_DEVICE_EVENT)
int BPF_PROG(ds4_probe_swallow_event, struct hid_bpf_ctx *hid_ctx)
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

HID_BPF_OPS(ds4_probe) = {
	.hid_device_event = (void *)ds4_probe_swallow_event,
	.hid_rdesc_fixup = (void *)ds4_probe_fix_rdesc,
};

char _license[] SEC("license") = "GPL";
