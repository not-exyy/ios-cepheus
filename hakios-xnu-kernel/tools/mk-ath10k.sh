#!/bin/bash
# Build the driver of the Mi 9's (cepheus) Wi-Fi radio for our kernel: ath10k, Qualcomm's driver for
# the WCN3990 (here a WCN3998, whose firmware runs in the modem's WLAN PD), as it is in Linux,
# unmodified, on the layer tools/mk-ath11k.sh builds ath11k on. Three things meet here:
#
#   Linux's ath10k        drivers/net/wireless/ath/ath10k of the release tools/mk-ath11k.sh pins
#                         (ISC), taken from the archive that script keeps;
#   FreeBSD's LinuxKPI    the tree tools/mk-ath11k.sh unpacks and patches ("mk-ath11k.sh freebsd"),
#                         with the headers its build would have generated;
#   wifi/lx               ours: of src/ the files that are no one driver's (the ticks, timers,
#                         memory, work; what stands in for mac80211; WPA), and of ath10k/ what is
#                         this driver's alone -- the headers its bus (snoc.c) wants that LinuxKPI
#                         lacks or has as nothing but names (ath10k/include, read before the
#                         layer's own), and the platform the bus is probed on (ath10k/*.c).
#
# Not all of the driver is built: DRIVER below lists the files that are, the ones its Makefile
# builds for the bus "snoc" with neither debugfs, tracing, the LEDs, thermal, spectral scans,
# test mode nor coredumps. What they call in the files that are not becomes a function that says
# its name in the log and answers 0 (out/stubs.txt lists them), unless wifi/lx has one that does more.
#
# What comes out, in xnu-deps/build/ath10k:
#   driver/ath10k/     the driver's files, untouched       driver/        what it takes of ath/
#   out/ath10k.o       the object for the kernel           out/stubs.txt  what is not there yet
# Needs what tools/mk-ath11k.sh needs.
#
# usage: tools/mk-ath10k.sh
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
[ "$(uname)" = Darwin ] && PATH=$ROOT/tools/macos-bin:$PATH:/Library/Developer/CommandLineTools/usr/bin
"$ROOT/tools/mk-ath11k.sh" freebsd > /dev/null
VERSION=$(sed -n 's/^VERSION=//p' "$ROOT/tools/mk-ath11k.sh")
A=$ROOT/xnu-deps/build/ath11k
B=$ROOT/xnu-deps/build/ath10k
S=$A/freebsd/sys
W=$ROOT/wifi/lx
ATH=drivers/net/wireless/ath
mkdir -p "$B"
cd "$B"

# the driver, and what it takes of the code all of Atheros' drivers share (ath/: the regulatory domains)
COMMON="ath.h reg.h regd.c regd.h regd_common.h dfs_pattern_detector.h dfs_pri_detector.h spectral_common.h trace.h"
if [ "$(cat driver/.release 2>/dev/null)" != "$VERSION $COMMON" ]; then
	rm -rf driver unpacked
	mkdir unpacked
	tar xJf "$A/linux-$VERSION.tar.xz" -C unpacked --strip-components=1 linux-$VERSION/$ATH/ath10k \
		$(for f in $COMMON; do echo linux-$VERSION/$ATH/$f; done)
	# every file must say itself that it may be used so: the licence is checked, not assumed
	for f in unpacked/$ATH/ath10k/*.[ch] $(for f in $COMMON; do echo unpacked/$ATH/$f; done); do
		case "$(head -6 "$f")" in
		*"SPDX-License-Identifier: ISC"*) ;;
		*"Permission to use, copy, modify, and/or distribute this software"*) ;;      # (ISC, spelt out)
		*) echo "mk-ath10k.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mv unpacked/$ATH driver
	rm -rf unpacked
	echo "$VERSION $COMMON" > driver/.release
fi

# --- compile: the driver's files that are built, the layer's that are no one driver's, and ours
DRIVER="core mac debug htc htt htt_rx htt_tx txrx wmi wmi-tlv bmi hw p2p swap ce qmi qmi_wlfw_v01 snoc"
ATHS="regd"
LAYER="lx_base lx_80211 lx_wpa"
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/ath10k/include -I$W/include -I$A/gen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Idriver/ath10k -Idriver -include $W/include/lx_prelude.h -include $W/ath10k/include/lx_ath10k.h -DCONFIG_ATH10K_DEBUG -DKBUILD_MODNAME=\"ath10k\""
source "$ROOT/xnu-env.sh"
for h in lx qmi; do
	[ -f "$XNU_SRC/bsd/sys/hakios.$h.h" ] || { echo "mk-ath10k.sh: the kernel tree has no bsd/sys/hakios.$h.h" >&2; exit 1; }
done
DFLAGS="$DFLAGS -DLX_HAKIOS_LX_H=\"$XNU_SRC/bsd/sys/hakios.lx.h\" -DLX_HAKIOS_QMI_H=\"$XNU_SRC/bsd/sys/hakios.qmi.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
for f in $DRIVER; do compile driver/ath10k/$f.c obj/ath10k_$f.o $DFLAGS & done
for f in $ATHS; do compile driver/$f.c obj/ath_$f.o $DFLAGS & done
OURS=$(cd "$W/ath10k" && ls *.c | sed 's/\.c$//')
for f in $LAYER; do compile "$W/src/$f.c" obj/$f.o $DFLAGS -I$W/src & done
for f in $OURS; do compile "$W/ath10k/$f.c" obj/$f.o $DFLAGS -I$W/src & done
wait
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
OBJECTS=$(for f in $DRIVER; do echo obj/ath10k_$f.o; done; for f in $ATHS; do echo obj/ath_$f.o; done; for f in $LAYER $OURS; do echo obj/$f.o; done)

# --- tie it all into one object, as tools/mk-ath11k.sh does: what it then still wants is a call of
# the kernel's, a function of a part of the driver that is not built (a stub each, that says its
# name and answers 0), or a mistake, which ends the build
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
llvm-nm -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|qmi_*|hakios_qmi_*|section\$*) continue ;;
	ath10k_*|__ath10k_*) ;;
	*) echo "mk-ath10k.sh: nothing defines $s" >&2; missing=1; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
[ -z "$missing" ] || exit 1
$CC -c out/stubs.s -o out/stubs.o
# four names are shown: where the kernel enters (lx_prepare(), lx_start(), lx_stop() and lx_transmit() of hakios.lx.h)
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/ath10k.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-ath10k.sh: out/ath10k.o: $(llvm-size out/ath10k.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$(echo $DRIVER | wc -w) of the driver's files; $(wc -l < out/stubs.txt) functions of the others are stubs (out/stubs.txt);" \
	"asks the kernel for $(llvm-nm -u --format=just-symbols out/ath10k.o | grep -vc 'section\$') names"
