#!/bin/sh
# Safety net for the default HID-unbind hide method (see TODO.md and
# src/hid-unbind-detect.*): rebinds any DualShock 4/DualSense hid_device
# left driverless by a daemon crash or SIGKILL, which skips every one of
# the daemon's own rebind-on-exit paths (see main.cpp's final shutdown
# block, the release-physical IPC handler, and the type-switch handler --
# all three write "bind" back via sysfs on a clean exit, but none of that
# code ever runs if the process is killed instead of signaled).
#
# Run from ds4-translator.service's ExecStartPre and ExecStopPost, so it
# fires both before a fresh start and after every stop, clean or not.
# Idempotent and a no-op on a clean daemon exit: everything is already
# rebound by then, so this finds nothing left to do.
for dev in /sys/bus/hid/devices/*054C*; do
    [ -e "$dev" ] || continue
    id=$(basename "$dev")
    case "$id" in
        0003:054C:05C4.*|0003:054C:09CC.*|0005:054C:05C4.*|0005:054C:09CC.*) ;;
        *) continue ;;
    esac
    [ -e "$dev/driver" ] && continue # already bound, nothing to do

    resolved=$(readlink -f "$dev")
    case "$resolved" in
        *dummy_hcd*) continue ;; # this daemon's own FunctionFS gadget, never rebind it
    esac
    case "$id" in
        0003:*)
            case "$resolved" in
                */devices/virtual/misc/uhid/*) continue ;; # this daemon's own uhid backend
            esac
            ;;
    esac

    echo "$id" > /sys/bus/hid/drivers/playstation/bind 2>/dev/null
done

exit 0
