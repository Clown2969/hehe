#!/bin/busybox sh
/bin/busybox --install -s
mount -t proc none /proc >/dev/null 2>&1
mount -t sysfs none /sys >/dev/null 2>&1
mount -t devtmpfs none /dev >/dev/null 2>&1

insmod /brd.ko rd_size=1024 rd_nr=1 >/dev/null 2>&1
mdev -s >/dev/null 2>&1

DISK="ram0"
insmod /solution.ko disk_name=/dev/$DISK sb1_offset=0 sb2_offset=8 max_name_len=32 max_file_sectors=1 format=1 >/dev/null 2>&1

mkdir -p /mnt
mount -t simplefs /dev/$DISK /mnt >/dev/null 2>&1

exec /bin/getty -L 115200 ttyS0 vt100 -n -l /bin/sh
