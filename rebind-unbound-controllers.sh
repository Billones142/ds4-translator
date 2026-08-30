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
    # The unlink hide method parks Bluetooth controllers on hid-generic so
    # hid-playstation never registers their battery/LED/force-feedback
    # (see swap_bt_driver_to_generic() in main.cpp). A crash skips the
    # hand-back just like it skips the rebind below, leaving the pad bound
    # but stripped of those features, so treat it the same as driverless.
    if [ -e "$dev/driver" ]; then
        [ "$(basename "$(readlink -f "$dev/driver")")" = "hid-generic" ] || continue
        case "$id" in
            0005:*) ;;
            *) continue ;; # only ever swapped on Bluetooth
        esac
        echo "$id" > /sys/bus/hid/drivers/hid-generic/unbind 2>/dev/null
    fi

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

    # Can legitimately fail while the daemon's own emulated device is still
    # registered with the physical pad's MAC (the unlink method's MAC squat --
    # see select_virtual_mac() in main.cpp): hid-playstation refuses a second
    # device for a MAC it already holds. A uhid emulated device dies with the
    # process, but a FunctionFS one lives in configfs and can outlive a
    # SIGKILL, in which case the pad stays driverless until the gadget is torn
    # down and this script runs again.
    echo "$id" > /sys/bus/hid/drivers/playstation/bind 2>/dev/null
done

exit 0
