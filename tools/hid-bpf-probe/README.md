# hid-bpf-probe

Phase 1 feasibility spike for the Bluetooth unbind-hide fix -- see
`/home/stefano/.claude/plans/search-on-the-internet-whimsical-possum.md`.
Standalone, **not** wired into the daemon. Goal: confirm a HID-BPF
`hid_device_event` program can swallow every DS4 report before hidraw/evdev
ever see it, while still delivering every report to this tool via a ring
buffer -- before committing to the real `src/hid-bpf-transport.*` integration.

## KNOWN DANGEROUS: do not touch `hid_rdesc_fixup` on this device

An earlier version of `probe.bpf.c` also implemented `hid_rdesc_fixup`,
zeroing the report descriptor down to a single byte to suppress evdev/js
node creation in addition to swallowing reports. Tested live against a
real Bluetooth DS4 (kernel 7.1.6-1-cachyos) on 2026-08-10: the kernel
logged `playstation ...: unknown main item tag 0x0` on reprobe (the
parser choking on the mangled descriptor), then crashed on the DS4's
very next input report:

```
BUG: kernel NULL pointer dereference, address: 0000000000000010
Oops: 0002 [#1] SMP PTI
RIP: 0010:memcpy_orig+0x29/0x130
Call Trace:
 dispatch_hid_bpf_device_event+0xd9/0x170
 __hid_input_report+0xad/0x280
 hid_safe_input_report+0x14/0x20
 uhid_char_write+0x316/0x6f0 [uhid]
 ...
note: bluetoothd[847] exited with irqs disabled
```

The oops happened inside the *kernel's own* HID-BPF dispatch code (not
this program's logic), most likely because the dispatcher precomputes
buffer/size bookkeeping from the probed descriptor and got a bogus
value from the 1-byte descriptor. It didn't panic the machine, but it
left `bluetoothd` wedged in unkillable D-state -- `systemctl restart
bluetooth` hung indefinitely, and the adapter was gone from D-Bus
(`bluetoothctl` reporting "No default controller available") until a
reboot.

The current `probe.bpf.c` only implements `hid_device_event` and does
**not** touch `hid_rdesc_fixup`. hidraw/evdev nodes for the device stay
visible with this version (just carrying no data, since every report is
swallowed into the ring buffer instead) -- weaker hiding than the
original Phase 1 goal, but doesn't crash the kernel. If a real fix needs
descriptor suppression too, it needs a fundamentally different approach
(e.g. a *valid*, minimal-but-parseable descriptor rather than a
truncated garbage one) and must be re-tested this carefully, ideally
against a kernel with `panic_on_oops=0` and a way to recover without a
full reboot, before ever running near a device anyone depends on.

## Prerequisites (this machine)

Already confirmed present: kernel 7.1.6-1-cachyos, `CONFIG_HID_BPF=y`,
`/sys/kernel/btf/vmlinux`, `clang`, `libbpf` 1.7.0 (pacman), `bpftool`
(pacman `bpf` package), `udev-hid-bpf` (pacman `udev-hid-bpf` package).

## Headers

`hid_bpf.h`, `hid_bpf_helpers.h`, and `hid_report_descriptor_helpers.h`
in this directory are vendored verbatim from udev-hid-bpf
(`gitlab.freedesktop.org/libevdev/udev-hid-bpf`, `src/bpf/`) -- not
packaged standalone anywhere, every HID-BPF fix vendors its own copy.
`vmlinux.h` is machine-generated (see Build below) and **not** committed
-- it's ~160k lines, regenerate it locally instead.

## Build

```sh
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

Note: redirect stdout only. `bpftool` prints an informational
`skipping /sys/kernel/btf/vmlinux (will be loaded as base)` line to
**stderr** -- if you redirect `2>&1` into the same file it lands as
line 1 of `vmlinux.h` and breaks the build with a nonsensical `unknown
type name 'skipping'` error.

```sh
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -I. -c probe.bpf.c -o probe.bpf.o
gcc -O2 -Wall -Wextra -DWITH_GZFILEOP probe.c -o probe -lbpf
```

(`-DWITH_GZFILEOP` matches this system's `pkg-config --cflags libbpf`
output -- check yours with that command rather than assuming.)

## Load and test (needs a real DS4 paired over Bluetooth)

```sh
sudo udev-hid-bpf --verbose add /sys/bus/hid/devices/<hid_id> probe.bpf.o
```

`<hid_id>` is the exact sysfs entry, e.g. `0005:054C:05C4.002F` (find it
with `ls /sys/bus/hid/devices/ | grep -i 054c`) -- confirmed the loader
does *not* accept a glob here (`0005:054C:*.*` fails with `Invalid
syspath`), unlike what an earlier version of this doc assumed.

The loader prints the ring buffer's actual pin path on success, e.g.:

```
libbpf: DEBUG Successfully pinned map at /sys/fs/bpf/hid/0005_054C_05C4_002F/probe_bpf/ds4_reports
```

Then, as root (the pinned bpffs object isn't readable otherwise):

```sh
sudo ./probe /sys/fs/bpf/hid/<hid_id_with_underscores>/probe_bpf/ds4_reports
```

While `probe` is running, in separate terminals:

- `evtest` (or open the DS4's `/dev/hidrawN` directly) should see **no**
  input at all once the program is attached -- proves other consumers are
  blind.
- `./probe`'s stdout should print every button/stick change live -- proves
  this daemon's future replacement transport would receive full data.
- Write an LED/rumble output report to the DS4's still-existing hidraw
  node (e.g. via `ds4-ctl` against a controller manually opened, or a
  short test write) and confirm it still reaches the controller --
  `hid_device_event` is inbound-only, so this should be unaffected, but
  needs confirming against real hardware, not assumed.

To confirm the hook is actually firing (struct_ops programs don't show
`run_cnt` in `bpftool prog show` even with
`sysctl -w kernel.bpf_stats_enabled=1`), add a temporary
`bpf_printk(...)` call inside `ds4_probe_swallow_event` and watch
`sudo cat /sys/kernel/tracing/trace_pipe`.

## Unload

```sh
sudo udev-hid-bpf remove /sys/bus/hid/devices/<hid_id>
```

## Status (2026-08-10)

Descriptor-suppression half of the mechanism (`hid_rdesc_fixup`)
crashed the kernel on first real-hardware test -- see the warning above.
Struck from `probe.bpf.c`.

The report-swallow-and-mirror half (`hid_device_event` + ringbuf) is
**verified working end-to-end** against a real Bluetooth DS4
(`0005:054C:05C4.0008`, same kernel, post-reboot), with the daemon's own
driver still normally bound throughout (no sysfs unbind involved at
all):

- `./probe` received a continuous stream of real 78-byte report-ID-0x11
  reports while the controller was moved/pressed -- confirms the
  ringbuf mirror path.
- `dd if=/dev/hidraw6` and `dd if=/dev/input/event31` (this device's
  hidraw and evdev nodes) both blocked for the full 8s test window with
  **zero bytes read**, in the same window `probe` was actively
  receiving data -- confirms every other consumer of this hid_device is
  fully blind, no permission-window race, no unbind needed.
- `udev-hid-bpf remove` cleanly detached the program; hidraw
  immediately resumed delivering normal reports afterward -- confirms
  clean teardown with the driver never having been touched.
- **Not yet tested:** outbound LED/rumble while the program is
  attached. `hid_device_event` is documented as inbound-only and the
  program never touches `hid_hw_request`/`hid_hw_output_report`, so
  this is expected to be unaffected, but that's not yet confirmed
  against real hardware the way the above three are.

Go/no-go for Phase 2: **go**, pending the LED/rumble check above.

Record the outcome (pass/fail on each of the three checks above) back
into the plan file before starting Phase 2 integration work.
