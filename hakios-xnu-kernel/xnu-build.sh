#!/bin/bash
# Build XNU for sm7325 on Linux. Extra make args are passed through.
source "$(dirname "${BASH_SOURCE[0]}")/xnu-env.sh"
cd "$XNU_SRC" || exit 1
# The Wi-Fi driver's object is linked into the kernel when it is there; make does not see it
# change, so the linked kernel is thrown away when it has. Which one is the phone's radio's:
# ath11k (tools/mk-ath11k.sh) with CONFIG_HAKIOS_WIFI, ath10k (tools/mk-ath10k.sh) with
# CONFIG_HAKIOS_WCN3990 -- each only with CONFIG_HAKIOS_WLAN as well, whose hakios.lx.c answers its calls.
LX=$XNUDEPS/build/ath11k/out/ath11k.o
if [ -f config/MASTER.hakios ]; then
	drivers=$(sed -n 's/^#  HAKIOS_DRIVERS = \[\(.*\)\]/ \1 /p' config/MASTER.hakios)
	case "$drivers" in
	*" hakios_wlan "*)
		case "$drivers" in
		*" hakios_wifi "*) ;;
		*" hakios_wcn3990 "*) LX=$XNUDEPS/build/ath10k/out/ath10k.o ;;
		*) LX=/nonexistent ;;
		esac ;;
	*) LX=/nonexistent ;;
	esac
fi
STAMP=BUILD/obj/hakios-objects
mtime() { if [ "$XNU_HOST_OS" = Darwin ]; then stat -f %m "$1"; else stat -c %Y "$1"; fi; }
if [ -f "$LX" ]; then now="$LX $(mtime "$LX")"; else LX= now=; fi
# the build tools of SETUP/ are Apple's: on macOS they build as they are, on Linux with a compatibility header
if [ "$XNU_HOST_OS" = Darwin ]; then HOSTCC=clang; else
HOSTCC="clang -include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration"; fi
if [ -d BUILD/obj ] && [ "$(cat $STAMP 2>/dev/null)" != "$now" ]; then
	rm -f BUILD/obj/*/mach.*.unstripped
	echo "$now" > $STAMP
fi
exec make HAKIOS_OBJECTS="$LX" SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-BCM2837} KERNEL_CONFIGS=DEVELOPMENT \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 \
  HOST_CC="$HOSTCC" HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n "$@"
