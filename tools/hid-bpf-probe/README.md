# hid-bpf-probe

Phase 1 feasibility spike for the Bluetooth unbind-hide fix -- see
`/home/stefano/.claude/plans/search-on-the-internet-whimsical-possum.md`.
Standalone, **not** wired into the daemon. Goal: confirm a HID-BPF
`hid_device_event` program can swallow every DS4 report before hidraw/evdev
ever see it, while still delivering every report to this tool via a ring
buffer -- before committing to the real `src/hid-bpf-transport.*` integration.

`probe.bpf.c` currently won't compile as-is: it depends on two things this
repo doesn't vendor yet, listed below. That's expected for this stage --
`vmlinux.h` not found and `HID_DEVICE`/`BPF_MAP_TYPE_RINGBUF`/etc undeclared
are the missing-header errors, not a bug in the source.

## Prerequisites (this machine)

Already confirmed present: kernel 7.1.6-1-cachyos, `CONFIG_HID_BPF=y`,
`/sys/kernel/btf/vmlinux`, `clang`, `libbpf` 1.7.0 (pacman).

Still needed:

```sh
sudo pacman -S bpf            # provides bpftool, for vmlinux.h
sudo pacman -S udev-hid-bpf   # the loader CLI (attaches struct_ops by
                               # HID_DEVICE() match, no hand-rolled
                               # attach code needed for this spike)
```

## Missing headers

`probe.bpf.c` includes `hid_bpf.h` and `hid_bpf_helpers.h` for the
`HID_BPF_CONFIG`/`HID_DEVICE`/`HID_BPF_OPS`/`hid_bpf_get_data` macros and
kfunc declarations. These aren't packaged standalone anywhere -- every
HID-BPF fix (including udev-hid-bpf's own bundled fixes) vendors its own
copy from the udev-hid-bpf source tree's `src/bpf/` directory. Grab them
from there (e.g. `github.com/bentiss/udev-hid-bpf`, `src/bpf/hid_bpf.h`
and `src/bpf/hid_bpf_helpers.h`) and drop them in this directory next to
`probe.bpf.c` before building.

## Build

```sh
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
    -I. -c probe.bpf.c -o probe.bpf.o
gcc -O2 -Wall -Wextra $(pkg-config --cflags libbpf) \
    probe.c -o probe $(pkg-config --libs libbpf)
```

## Load and test (needs a real DS4 paired over Bluetooth)

```sh
sudo udev-hid-bpf add /sys/bus/hid/devices/0005:054C:*.* probe.bpf.o
./probe
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

## Unload

```sh
sudo udev-hid-bpf remove /sys/bus/hid/devices/0005:054C:*.*
sudo rm -f /sys/fs/bpf/ds4_reports
```

Record the outcome (pass/fail on each of the three checks above) back into
the plan file before starting Phase 2 integration work.
