#!/system/bin/sh
# rc.boot — run once by init, after the kernel filesystems are mounted and
# before any console starts. Boot policy lives here so it can change without
# recompiling init.
export PATH=/system/bin

. /config/os-release
echo "Starting $PRETTY_NAME"

hostname -F /config/hostname

# --- Hardware --------------------------------------------------------------
# Firmware for Wi-Fi cards and GPUs lives in /system/firmware.
if [ -w /sys/module/firmware_class/parameters/path ]; then
    echo /system/firmware > /sys/module/firmware_class/parameters/path
fi

# Hotplug: devices that appear later (USB sticks...) get their driver too.
echo /system/bin/mdev > /proc/sys/kernel/hotplug

# Coldplug: load drivers for every device already present.
kver="$(uname -r)"
if [ -f "/system/modules/$kver/modules.dep" ]; then
    find /sys/devices -name modalias -type f -exec cat {} + 2>/dev/null \
        | sort -u | xargs -r modprobe -qa 2>/dev/null
fi
mdev -s

# --- Network ---------------------------------------------------------------
ip link set lo up
: > /run/resolv.conf
for dev in /sys/class/net/*; do
    name="${dev##*/}"
    case "$name" in lo) continue ;; esac
    [ -d "$dev/wireless" ] && continue          # Wi-Fi needs credentials first
    ip link set "$name" up
    udhcpc -b -q -i "$name" >/dev/null 2>&1 &
done

# --- Self-test (kernel command line: ve.test=1) ----------------------------
case " $(cat /proc/cmdline) " in
    *" ve.test=1 "*)
        /system/lib/boot/selftest
        poweroff
        ;;
esac
