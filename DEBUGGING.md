# Debugging & Interceptor Guide for `ds4-translator`

This guide explains how to diagnose controller detection issues with games, inspect `ds4-translator` service behavior, and use the diagnostic `libds4-intercept` library.

---

## 1. Quick Service Diagnostics

### Check Translator Service Status
Use `ds4-ctl` to verify the state of physical controller detection and virtual emulation:

```bash
ds4-ctl status
```
**Example Output:**
```text
Physical Controller: /dev/hidraw6
Connection Type: USB
Virtual Emulation: DualShock 4
Device Open by Host: Yes
```

### View Service Logs
Inspect the live background service logs (does not require root privileges):

```bash
journalctl -u ds4-translator -n 50 -f
```

### Inspect Virtual & Physical Device Properties
Check the udev properties assigned to an input or hidraw node:

```bash
udevadm info -q all /dev/hidraw6
udevadm info -q all /dev/input/event256
```

---

## 2. Using the Diagnostic Interceptor (`libds4-intercept.so`)

`libds4-intercept.so` is a lightweight `LD_PRELOAD` diagnostic tool designed to trace low-level system calls between games and controller device nodes (`/dev/hidraw*`, `/dev/input/event*`, `/dev/input/js*`).

### Building the Interceptor
The interceptor library is opt-in and can be built via:

```bash
make intercept
```
This generates:
- `libds4-intercept.so` (64-bit applications / native games)
- `libds4-intercept32.so` (32-bit applications)

---

### Running with a Native Application / Game

Launch the application with `LD_PRELOAD`:

```bash
LD_PRELOAD=/path/to/libds4-intercept.so ./your-game
```

By default, logs are written to `/tmp/ds4-intercept.log`. To specify a custom log file location:

```bash
DS4_INTERCEPT_LOG=/path/to/my-debug.log LD_PRELOAD=/path/to/libds4-intercept.so ./your-game
```

---

### Running with a Steam / Proton Game

To trace a game launched through Steam:

1. Open Steam, right-click the game, and select **Properties**.
2. Under **General** -> **Launch Options**, enter:
   ```text
   LD_PRELOAD=/path/to/libds4-intercept.so %command%
   ```
3. Launch the game and reproduce the issue.
4. Inspect `/tmp/ds4-intercept.log` (or your custom `DS4_INTERCEPT_LOG` path).

---

## 3. Interpreting Interceptor Logs

The log records timestamps, thread IDs, and system call events:

### Event Types Logged

| Log Prefix | Meaning |
| :--- | :--- |
| `OPEN /dev/...` | Game opened a device node (lists `fd` or error code, e.g., `errno=13 Permission denied`) |
| `READ fd=X` | Game read an input report from hidraw/evdev (includes hex dump) |
| `WRITE fd=X` | Game sent an output report (rumble / LED color) to hidraw (includes hex dump) |
| `IOCTL fd=X` | Game issued an ioctl request (decoded, e.g., `HIDIOCGFEATURE`, `EVIOCGNAME`, `EVIOCGID`) |
| `POLL fd=X` | Game polled a controller file descriptor for readiness |
| `CLOSE fd=X` | Game closed a device node |

### Example Log Entries

```text
[18011.739] [tid=507899] OPEN /dev/hidraw6 → fd=45 (type=hidraw, flags=0x2)
[18011.951] [tid=507899] IOCTL fd=45 (hidraw /dev/hidraw6) HIDIOCGRDESCSIZE → ret=0
[18011.951] [tid=507899]   → descriptor size=499
[18012.032] [tid=507904] IOCTL fd=79 (hidraw /dev/hidraw6) HIDIOCGFEATURE(64) → ret=16
[18070.858] [tid=507897] WRITE fd=45 (hidraw /dev/hidraw6) 32 bytes
[18070.858] [tid=507897]   data (32 bytes): 05 01 00 00 00 00 00 00 ...
```

---

## 4. Common Issue Patterns

1. **`Permission denied (errno=13)` on `/dev/hidraw*`**:
   - Indicates udev permissions or `uaccess` tags are missing for the current user.
   - Solution: Check `/etc/udev/rules.d/72-ds4-translator-hide.rules`.

2. **Game opens `/dev/hidraw*` but ignores input**:
   - Search the log for `HIDIOCGFEATURE`. Look at which Feature Report IDs (e.g. `0x02`, `0x10`, `0x12`, `0x25`) the game is requesting.
   - If `HIDIOCGFEATURE` fails (`ret=-1`), `ds4-translator` needs to be updated to answer that specific Feature Report in `src/main.cpp`.

3. **Controller detected as Generic Xbox Controller**:
   - Check if `libudev` properties (`ID_BUS=usb`, `ID_VENDOR_ID=054c`, `ID_MODEL_ID=05c4`) are correctly injected into the virtual device using `udevadm info -q all /dev/hidrawX`.
