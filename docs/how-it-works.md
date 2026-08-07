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

---

## 3. Emulating the Virtual Controller

The daemon uses the kernel's User-space HID (`/dev/uhid`) driver to create a virtual USB device:
* **Report Descriptors**: Emulates either a standard DualShock 4 USB controller or a DualSense Wireless Controller connected via USB using their authentic HID report descriptors.
* **Firmware & Identity Matching**:
  - The virtual controller reports the official hardware version `0x8111` in `UHID_CREATE2`.
  - It exposes a virtual MAC address (`74:e7:d6:3a:47:e8`) that matches the `uniq` field in UHID registration and the responses to Feature Reports `0x09` (DualSense pairing) and `0x12` (DS4 pairing).
  - It handles `UHID_GET_REPORT` and responds to Feature Reports `0x05` (calibration data) and `0x20` (firmware info) with authentic byte payloads from real controllers. This satisfies game security checks (like those in native PlayStation PC ports like *Jedi: Survivor*) which would otherwise reject mock devices.

### 3a. Alternative Backends (experimental)

UHID skips real USB enumeration entirely — the kernel's HID subsystem
receives a fully-formed device with no `GET_DESCRIPTOR`/`SET_CONFIGURATION`
handshake. Two alternative backends instead present a genuinely enumerated
USB device, selectable at runtime via `ds4-ctl set-backend <uhid|gadget|hidg>`
(persisted to `/etc/ds4-translator.conf` as `backend=...`, alongside `type=`):

* **`gadget`** (`src/raw-gadget-backend.c`): uses `/dev/raw-gadget` bound to
  the `dummy_hcd` kernel module's `dummy_udc.0`, hand-rolling every USB
  control-transfer response (device/config/string/HID/report descriptors,
  `SET_CONFIGURATION`, `SET_REPORT`, `SET_IDLE`) itself.
* **`hidg`** (`src/hidg-backend.c`): builds the same USB gadget via configfs
  (`/sys/kernel/config/usb_gadget/`) using the kernel's built-in `f_hid`
  function, bound to the same `dummy_udc.0`. The kernel handles the USB
  control-transfer handshake itself; userspace only reads/writes HID reports
  on the resulting `/dev/hidgN` node.

  It answers control-transfer GET_REPORT requests (calibration data, pairing
  info — see section 3 above) via the `GADGET_HID_READ_GET_REPORT_ID`/
  `GADGET_HID_WRITE_GET_REPORT` ioctls (`linux/usb/g_hid.h`) with the same
  authentic payloads UHID and `gadget` serve.

  **Known limitation**: those ioctls were added to `f_hid` around 2022; on
  older kernels without them, `hidg-backend.c` detects the unsupported ioctl
  at startup and falls back to the kernel's built-in zero-filled/stall
  response for feature reports instead.

Both are opt-in and fall back to `uhid` automatically if device creation
fails (`create_virtual_device()` in `main.cpp`). Neither has been verified
end-to-end against real hardware — see README Known Issues.

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

