#!/bin/bash
# adb for hakios-xnu-kernel/tools/backup-partitions.py while the phone is in TWRP: the shell there is root already and
# has no `su`. "exec-out su -c CMD" becomes "exec-out CMD"; everything else goes to adb as it is.
ADB=${ADB:-adb}
if [ "$1" = exec-out ] && [ "$2" = su ] && [ "$3" = -c ]; then
	shift 3
	exec "$ADB" exec-out "$@"
fi
exec "$ADB" "$@"
