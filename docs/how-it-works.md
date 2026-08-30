# How DS4-Translator Works

This document describes the architecture, mechanics, and design decisions behind the `ds4-translator` software.

---

## 1. System Architecture

The software consists of three primary components:
1. **The Daemon (`ds4-translator`)**: A root-level service that captures physical DualShock 4 inputs and translates them into a virtual USB gamepad.
2. **The Control Utility (`ds4-ctl`)**: A user-space CLI program that communicates with the daemon at runtime via a Unix Domain Socket.
3. **The Persistent Configuration (`/etc/ds4-translator.conf`)**: A static configuration file read at startup and written dynamically when configuration changes are requested.

```mermaid
graph TD
    A[Physical DualShock 4] -- Capture HID/Evdev Nodes --> B(ds4-translator Daemon)
    B -- Emulate USB HID Reports --> C[Virtual Controller via /dev/uhid]
    D[ds4-ctl CLI] -- IPC Socket commands --> B
    B -- Read/Write Config --> E[/etc/ds4-translator.conf]
```

---

## 2. Capturing & Hiding the Physical Controller

To prevent games and launchers (like Steam) from receiving double-input or registering the physical controller alongside the virtual one, the daemon completely hides the physical device:

1. **Capturing `hidraw`**: The daemon scans `/dev/hidraw*` to detect a Sony DualShock 4 device (Bluetooth or USB).
2. **Hiding nodes via udev Rules (`72-ds4-translator-hide.rules`)**:
   - A custom udev rule `/etc/udev/rules.d/72-ds4-translator-hide.rules` matches the physical DualShock 4 device (by Bluetooth kernel name pattern `0005:054C:05C4.*`, or by the USB device's own `ATTRS{idVendor}`/`ATTRS{idProduct}` — not `DRIVERS=="usb"`, since on kernels using `hid-playstation` the intermediate hid-class device's driver is `playstation`/`sony`, not `usb`, so a combined `KERNELS=="0003:054C:*"` + `DRIVERS=="usb"` match never resolves to the same ancestor device and silently never fires).
   - The rule runs after default tag rules but before late seat rules. It strips the `uaccess` and `seat` tags (`TAG-="uaccess"`, `TAG-="seat"`) and sets permissions to `0600` owned by `root:root`.
   - Stripping the `uaccess` tag prevents `systemd-logind` from assigning POSIX Access Control Lists (ACLs) to the active logged-in user. This ensures Steam (running as the standard user) cannot open the device, even if it tries to open it immediately on hotplug.
3. **Hiding nodes via runtime permissions (`chmod 000`)**:
   - As a second layer, the daemon changes the permissions of the physical `/dev/hidrawX`, `/dev/input/event*`, and `/dev/input/js*` nodes to `000` (`c---------`).
   - Because the daemon runs as `root`, it retains full access to open and read from these nodes, whereas user-space processes (Steam, games, Wine) get a `Permission Denied` error.
4. **Exclusive Grab (`EVIOCGRAB`)**: The daemon opens the event nodes and invokes `ioctl(fd, EVIOCGRAB, 1)`. This tells the kernel to route all inputs exclusively to the daemon.
5. **Restoration on Exit**: When the physical controller disconnects or the daemon is stopped, the daemon restores the original file permission modes (usually `0660` / `0664`) of all hidraw, event, and joystick nodes so they return to normal.

### 2a. Hide Methods (`ds4-ctl set-hide-method`)

Which of the mechanisms above actually runs is selected by `hide_method=` in
`/etc/ds4-translator.conf` (`ds4-ctl set-hide-method <legacy|unbind|unlink>`).
It applies on the next physical (re)connection.

| Method | USB | Bluetooth |
| --- | --- | --- |
| `legacy` | `chmod 0600` + `setfacl -b` + `EVIOCGRAB`; nodes exist, permission-blocked | same |
| `unbind` (default) | kernel HID driver fully unbound, **no node ever created**; read via libusb | driver stays bound, nodes exist but are inert (HID-BPF swallows every report); siblings also chmod'd + grabbed |
| `unlink` | identical to `unbind` — there is no node to unlink | `unbind`, plus the pad is moved onto `hid-generic` and the leftover `hidraw`/`event*`/`js*` `/dev` entries are deleted outright |

The asymmetry is forced by BlueZ: the Bluetooth `hid_device` is created by
`bluetoothd`'s input plugin writing into `/dev/uhid`, and taking the L2CAP
session away from `bluetoothd` requires dropping the device's ACL link
entirely — so the driver cannot simply be unbound the way the USB side is.
`unbind` therefore neutralizes the nodes instead of removing them; `unlink`
removes them.

**How `unlink` works.** `/dev` is `devtmpfs`, where root can `unlink()` a
device node like any other file. The unlink drops only the *path*: the
character device itself is untouched, already-open descriptors (the daemon's
included) keep working, and the device stays listed in sysfs. What disappears
is the only handle other processes have on it — `libudev` enumeration still
reports the device, but the `DEVNAME` it hands out now `open()`s to `ENOENT`,
which Steam/SDL/the browser Gamepad API treat as "device gone" rather than
listing a second, frozen gamepad. The `chmod`/`EVIOCGRAB` steps still run
first, so they remain in force as a fallback.

Node identity (`st_rdev`, mode, uid, gid) is captured before the unlink and
replayed via `mknod` on teardown, so stopping the daemon is enough to get the
pad back — `udev` would eventually recreate the node on the device's next
uevent, but a Bluetooth controller that stays connected across a daemon
restart never generates one.

**The `hid-generic` swap.** Unlinking `/dev` entries doesn't reach everything
`hid-playstation` creates: it also registers a `power_supply`
(`ps-controller-battery-<MAC>`), an LED class device and a force-feedback
device, none of which have a `/dev` node to remove. Because Bluetooth keeps
the driver bound, the *physical* pad's battery therefore shows up in the
desktop's battery indicator next to the emulated device's own.

Simply unbinding the driver doesn't work: HID-BPF — the only Bluetooth
transport available here — needs a driver bound to receive reports at all,
so a driverless device is invisible to this daemon too. That was tried and
rejected in `9aa821c`.

`unlink` instead moves the device onto a *different* driver: `hid-generic`,
which registers none of those extras. So the battery never exists, while
HID-BPF keeps working because a driver is still bound. On release the device
is handed back to `hid-playstation`.

`hid-generic` does not accept the device on its own, though. Its
`hid_generic_match()` refuses anything listed in the kernel's
`hid_have_special_driver` table — which is exactly the set of devices
`hid-playstation` claims — so a bare write to its `bind` file returns
`ENODEV`. The only override is hid's `ignore_special_drivers` module
parameter, so the daemon raises
`/sys/module/hid/parameters/ignore_special_drivers` for the duration of the
single `bind` write and restores the previous value immediately after. That
parameter is global, so any *other* HID device probing inside that window
would also land on `hid-generic`; the hid bus has no per-device
`driver_override`, so there is no narrower lever available.

One consequence has to be compensated for: a Bluetooth DS4 only emits the
short `0x01` input report (no gyro, accelerometer or touchpad) until
something reads feature report `0x02`. `hid-playstation` does that in
`probe()`; `hid-generic` doesn't, so the daemon issues that `HIDIOCGFEATURE`
itself right after the swap.

The swap is best-effort throughout — any failure hands the device straight
back to `hid-playstation`, and the connection behaves exactly like the plain
`unbind` method. `rebind-unbound-controllers.sh` (wired into the service's
`ExecStartPre`/`ExecStopPost`) also treats a Bluetooth controller left on
`hid-generic` as needing repair, since a crash skips the hand-back the same
way it skips the existing rebind-on-exit paths.

Two known limits:
- Anything that reads `major:minor` out of sysfs and `mknod`s its own node
  still reaches the device. No real gamepad consumer does this.
- `udev` recreates a missing node on the next `add`/`change` uevent for the
  device, including ones the daemon causes itself (the delayed `udevadm
  trigger` after virtual device creation, the bare `udevadm trigger` on type
  changes). A 500 ms sweep in the main loop (`reapply_unlinked_nodes()`)
  re-unlinks anything that came back, so the hiding is sticky rather than
  silently lapsing.

---

## 3. Emulating the Virtual Controller

The daemon uses the kernel's User-space HID (`/dev/uhid`) driver to create a virtual USB device:
* **Report Descriptors**: Emulates either a standard DualShock 4 USB controller or a DualSense Wireless Controller connected via USB using their authentic HID report descriptors.
* **Firmware & Identity Matching**:
  - The virtual controller reports the official hardware version `0x8111` in `UHID_CREATE2`.
  - It exposes a virtual MAC address (`74:e7:d6:3a:47:e8`) that matches the `uniq` field in UHID registration and the responses to Feature Reports `0x09` (DualSense pairing) and `0x12` (DS4 pairing).
  - It handles `UHID_GET_REPORT` and responds to Feature Reports `0x05` (calibration data) and `0x20` (firmware info) with authentic byte payloads from real controllers. This satisfies game security checks (like those in native PlayStation PC ports like *Jedi: Survivor*) which would otherwise reject mock devices.

### 3a. Backend Selection (per controller type)

UHID skips real USB enumeration entirely — the kernel's HID subsystem
receives a fully-formed device with no `GET_DESCRIPTOR`/`SET_CONFIGURATION`
handshake. That's confirmed to matter: some Wine/Proton titles never detect
a DS4 emulated this way, no matter the configuration. `functionfs`
(`src/functionfs-backend.c`) instead presents a genuinely enumerated USB
device and is confirmed to fix that detection failure. DualSense emulation
never showed the issue on either backend.

DS4 and DualSense each have their own independent backend setting because
of this — `ds4-ctl set-backend <ds4|dualsense> <uhid|functionfs>`, persisted
to `/etc/ds4-translator.conf` as `backend_ds4=`/`backend_dualsense=`
alongside `type=`, defaulting to `ds4=functionfs`, `dualsense=uhid`. Either
type can still be pointed at either backend if wanted (`functionfs` works
for DualSense too); only the defaults differ.

`functionfs` builds the gadget via configfs
(`/sys/kernel/config/usb_gadget/`) using the generic `ffs` function, mounted
at `/dev/ffs-ds4emu0` and bound to the `dummy_hcd` kernel module's
`dummy_udc.0`. Standard requests (`GET_DESCRIPTOR` for device/config/string,
`SET_CONFIGURATION`, `SET_INTERFACE`) are answered by the gadget core itself
below FunctionFS — userspace (`ep0_loop()`) only sees the HID-specific class
requests (`GET_REPORT`/`SET_REPORT`/`SET_IDLE`) and the interface's own
`GET_DESCRIPTOR(HID_REPORT)`, read and answered directly as plain
`read()`/`write()` on `ep0`. Modeled after
[senseshock](https://github.com/muhammad23012009/senseshock)'s use of
FunctionFS for the same purpose (emulating a DS4 over a from-scratch USB
gadget), adapted to this project's own report descriptors and feature-report
tables (see section 3 above for the HW/FW version fields it answers with).

`functionfs` is opt-in and falls back to `uhid` automatically if device
creation fails (`create_virtual_device()` in `main.cpp`). It's the only
backend confirmed working end-to-end against real hardware — kernel's
`hid-playstation` driver binds to it, creates `hidraw`/input/Motion
Sensors/Touchpad nodes, and it's confirmed to fix DS4 detection in
Wine/Proton titles that never detected the `uhid` DS4.

Two earlier alternative backends (`gadget`, hand-rolled `/dev/raw-gadget`
ioctls; `hidg`, configfs + kernel `f_hid`) were removed after `functionfs`
proved out real-enumeration DS4 detection — an unrelated udev bug (see
`72-ds4-translator-hide.rules`'s `DEVPATH!=` guard) had silently kept both
of them from ever being reachable at all, so neither was ever actually
confirmed working, and `functionfs` already covers the same real-enumeration
path without their raw ioctl/kernel-source-reverse-engineering surface area.

---

## 4. Input & Output Translation

### Input Translation
* **HID Report Parsing**: The daemon reads raw input report packets from the physical controller.
* **Format Conversion**: It decodes button bitmasks, analog sticks, trigger positions, and trackpad coordinate states, and re-packages them into the output structure matching the active target emulation (DS4 or DualSense).

### Output Translation (Rumble & LED)
* **Output Capture**: The daemon listens for `UHID_OUTPUT` events sent by games/Proton to update controller state.
* **LED Passthrough**: Captures lightbar updates (RGB values) and translates them to the physical controller.
* **Rumble Heuristics (Sound-based haptics)**:
  - DualSense controllers use voice-coil actuators to play haptics as stereo sound signals. Traditional controllers like the DualShock 4 use spinning mass motors (ERM).
  - When the game triggers haptic vibrations, it sets the `DS_OUTPUT_VALID_FLAG0_HAPTICS_SELECT` flag. In this mode, both the heavy motor (low frequency) and light motor (high frequency) are often driven simultaneously.
  - On a DualShock 4, running both motors at the same time causes the heavy motor to completely overpower the light one, muddying the vibration.
  - The daemon resolves this with a heuristic: **when Haptics Select is active and both motors are triggered, the heavy motor is temporarily muted (`motor_left = 0`)**. This lets you feel the subtle, high-frequency steps and details handled by the light motor without it being drowned out.

---

## 5. Disabling Emulation dynamically (Type: `none`)

The software allows disabling emulation entirely via the command:
```bash
ds4-ctl set-type none
```

When changing the target type to `none`:
1. **Destroying Virtual Gamepad**: The daemon destroys any active virtual `/dev/uhid` gamepad device.
2. **Releasing Grabs & Permissions**:
   - The daemon releases any active `EVIOCGRAB` locks on the physical input event nodes.
   - It restores the physical controller's `hidraw`, `event*`, and `js*` nodes back to their original permission modes (usually `0660` or `0664`).
3. **Bypassing udev Hiding**:
   - The daemon creates a state file at `/run/ds4-translator.none`.
   - The custom udev rule `/etc/udev/rules.d/72-ds4-translator-hide.rules` includes the condition `TEST!="/run/ds4-translator.none"`.
   - The daemon triggers `udevadm trigger`. Since the `.none` file exists, the udev rules bypass the root-only lock and uaccess/seat tag removal.
   - This causes udev to assign standard `uaccess` tags and permissions, allowing Steam, browsers, and standard games direct access to the physical controller.
4. **Resuming Emulation**: When the emulation type is changed back to `ds4` or `dualsense`, the daemon unlinks `/run/ds4-translator.none`, triggers `udevadm trigger`, programmatically runs `setfacl -b` to clear any old user ACLs, and re-captures/re-hides the physical controller.

