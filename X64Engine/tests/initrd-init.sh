#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
echo
echo "=== x86-64 userspace running in the x64 Engine ==="
uname -a
cat /proc/cpuinfo | grep -m1 "model name"
echo "uptime: $(cat /proc/uptime)"
exec setsid cttyhack sh
