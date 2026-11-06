#!/bin/bash
# HakiOS: iOS 16.5 userland on the Xiaomi Mi 9 (cepheus), one-time setup. Read README.md first.
#
#   ./setup.sh check                    what is installed on this Mac, and the room on its disk
#   ./setup.sh firmware MIUI_ROM        the phone's own firmware (GPU, audio DSP) and its device tree, out of YOUR
#                                       MIUI fastboot ROM (a folder or the .tgz); nothing goes to the phone
#   ./setup.sh build IPSW               on the Mac only: the loader, the boot images and darwin.img from YOUR IPSW
#                                       (RAM_ONLY=1: without darwin.img, if you will only ever run from RAM)
#   ./setup.sh backup TWRP.img          the phone in fastboot -> TWRP from RAM -> a read-only copy of what cannot be
#                                       made again (partition tables, persist, modem calibration)
#   ./setup.sh install WHERE [TWRP.img] put iOS on the flash. WHERE:
#                                         new       a new partition "darwin" cut off the end of userdata (32 GiB,
#                                                   DARWIN_GIB=N to change); Android stays, its data is ERASED
#                                         userdata  the whole of userdata becomes "darwin" (51 GiB on a 64 GB phone);
#                                                   Android no longer starts until ./setup.sh uninstall
#                                         system, vendor: not possible, the command says why
#   ./setup.sh flash                    (in fastboot) write iOS into "darwin" again; /private/var is emptied
#   ./setup.sh verify TWRP.img          read "darwin" back in TWRP and compare it with darwin.img
#   ./setup.sh uninstall [TWRP.img]     put the partition table of the backup back: no "darwin", userdata whole
#                                       again (and erased once more)
#
# Then, every time: ./run.sh SCENARIO [ram]   (see ./run.sh)
set -euo pipefail
KIT=$(cd "$(dirname "$0")" && pwd)
R=$KIT/hakios-xnu-kernel
W=$KIT/work
export PATH="$KIT/platform-tools:$PATH"
export HAKIOS_VERSION_SUFFIX=${HAKIOS_VERSION_SUFFIX:--hakios-mi9}

IPSW_NAME=iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
IPSW_SIZE=6165698198
DARWIN_IMG_SIZE=8365539328         # Apple's root volume, then the OS and App cryptexes (2042368 blocks of 4096)
DARWIN_IMG_SHA=9efb156f417e32b6f92d709af33c0e8e584693c3fbd29eaf139b57eee3735560   # what the IPSW above gives
DARWIN_GIB=${DARWIN_GIB:-32}       # the size of a new "darwin": 8.4 GB of iOS, the rest is /private/var
DARWIN_MIN_BLOCKS=5242875          # what the kernel and the loader accept (20 GiB)
ROM_NAME=cepheus_global_images_V12.5.1.0.RFAMIXM   # the MIUI the kit was made on (its ABL starts HakiOS)

say() { printf '\n== %s\n' "$*"; }
die() { printf '\nОШИБКА: %s\n' "$*" >&2; exit 1; }
size() { python3 -c 'import os, sys; print(os.path.getsize(sys.argv[1]))' "$1"; }    # stat differs on macOS and Linux
sha() { shasum -a 256 "$@" | cut -d' ' -f1; }
confirm() {     # confirm "what will happen" WORD: nothing happens unless WORD is typed back
	printf '\n%s\nЧтобы продолжить, введите %s (что угодно другое — отмена): ' "$1" "$2"
	read -r answer
	[ "$answer" = "$2" ] || die "отменено, ничего не сделано"
}
need() { command -v "$1" >/dev/null 2>&1; }
sevenzip() { command -v 7zz || command -v 7z; }
fastboot_phone() {
	fastboot devices 2>/dev/null | grep -q fastboot || die "телефон не в fastboot (Громкость вниз + Питание на выключенном телефоне)"
	fastboot getvar product 2>&1 | grep -q "product: cepheus" ||
		die "в fastboot не Mi 9 (cepheus) или он в загрузчике HakiOS: $(fastboot getvar product 2>&1 | head -1)"
}
twrp_phone() {
	adb devices | grep -q 'recovery$' || die "adb не видит телефон в TWRP"
	[ "$(adb shell getprop ro.product.device | tr -d '\r')" = cepheus ] || die "в TWRP не Mi 9 (cepheus)"
}
boot_twrp() {   # boot_twrp TWRP.img COMMAND
	[ -f "${1:-}" ] || die "укажите образ TWRP для cepheus, например ./setup.sh $2 ~/Downloads/twrp-3.7.1_12-0-cepheus.img"
	fastboot_phone
	say "TWRP загружается из RAM (fastboot boot, ничего не прошивается)"
	fastboot boot "$1"
	for _ in $(seq 1 60); do adb devices | grep -q 'recovery$' && break; sleep 2; done
	twrp_phone
	adb shell 'test -b /dev/block/sda' || die "TWRP не видит /dev/block/sda"
}
in_twrp() { adb devices 2>/dev/null | grep -q 'recovery$' || boot_twrp "${1:-}" "$2"; twrp_phone; }
to_fastboot() {
	adb reboot bootloader
	for _ in $(seq 1 60); do fastboot devices 2>/dev/null | grep -q fastboot && break; sleep 2; done
	fastboot_phone
}
mkfs_hfs() { if [ "$(uname)" = Darwin ]; then echo "$R/tools/newfs-hfs-file.sh"; else echo mkfs.hfsplus; fi; }
last_backup() { ls -d "$KIT"/backups/cepheus-*/main 2>/dev/null | tail -1; }
lun0_blocks() { echo $(( $(adb exec-out 'blockdev --getsz /dev/block/sda' | tr -d '\r') / 8 )); }
lun0_sha() {    # lun0_sha head|tail BLOCKS: the sha256 of the first or the last 8 blocks of LUN 0 on the phone
	if [ "$1" = head ]; then adb exec-out 'dd if=/dev/block/sda bs=4096 count=8 2>/dev/null' | sha
	else adb exec-out "dd if=/dev/block/sda bs=4096 skip=$(($2 - 8)) count=8 2>/dev/null" | sha; fi
}

cmd_check() {
	local bad=0 free
	say "программы"
	for c in python3 clang lld-link adb fastboot shasum; do
		if need $c; then echo "  есть  $c"; else echo "  НЕТ   $c"; bad=1; fi
	done
	if need 7zz || need 7z; then echo "  есть  7z"; else echo "  НЕТ   7z (brew install sevenzip)"; bad=1; fi
	if [ "$(uname)" = Darwin ]; then
		[ -x /sbin/newfs_hfs ] && need hdiutil && echo "  есть  newfs_hfs, hdiutil" || { echo "  НЕТ   newfs_hfs/hdiutil"; bad=1; }
		[ -f /System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib ] &&
			echo "  есть  QuartzCore default.metallib этого Mac (для сценария metal)" ||
			echo "  нет   QuartzCore default.metallib: сценарий metal собрать не получится, gl и basic работают"
	else
		need mkfs.hfsplus && echo "  есть  mkfs.hfsplus" || { echo "  НЕТ   mkfs.hfsplus (hfsprogs)"; bad=1; }
		echo "  (Linux: сценарий metal не собирается, ему нужен macOS)"
	fi
	need lld-link || echo "        lld-link: brew install lld (или llvm)"
	need adb || echo "        adb/fastboot: brew install --cask android-platform-tools, или положите их в $KIT/platform-tools"
	free=$(df -Pk "$KIT" | awk 'NR == 2 { print int($4 / 1048576) }')
	say "место на диске: $free ГБ свободно (нужно около 30 на время build, 20 после)"
	[ "$free" -ge 30 ] || { echo "  МАЛО"; bad=1; }
	say "прошивка телефона и дерево устройств (./setup.sh firmware)"
	for f in fw-gpu/a640_gmu.bin fw-audio/adsp.mdt devices/cepheus/prebuilt/device-tree.dtb; do
		[ -f "$R/$f" ] && echo "  есть  $f" || echo "  нет   $f"
	done
	[ $bad = 0 ] && say "программы на месте" || die "чего-то не хватает (см. выше)"
}

# --- the phone's own firmware and device tree, from the user's own copy of MIUI -------------------------------------
cmd_firmware() {
	local rom=${1:-} img z t
	[ -n "$rom" ] && [ -e "$rom" ] || die "укажите прошивку MIUI для fastboot: папку ${ROM_NAME}_…_global или её .tgz"
	z=$(sevenzip) || die "нет 7z (brew install sevenzip)"
	t=$W/rom
	rm -rf "$t"; mkdir -p "$t"
	if [ -d "$rom" ]; then
		img=$rom/images
		[ -d "$img" ] || img=$rom
	else
		say "распаковка boot.img, vendor.img и NON-HLOS.bin из $(basename "$rom") (около 2 ГБ, несколько минут)"
		tar -xzf "$rom" -C "$t" --include='*/images/boot.img' --include='*/images/vendor.img' --include='*/images/NON-HLOS.bin' 2>/dev/null ||
			tar -xzf "$rom" -C "$t" --wildcards '*/images/boot.img' '*/images/vendor.img' '*/images/NON-HLOS.bin'
		img=$(dirname "$(find "$t" -name boot.img | head -1)")
	fi
	for f in boot.img vendor.img NON-HLOS.bin; do [ -f "$img/$f" ] || die "нет $img/$f: это не прошивка MIUI для fastboot"; done
	case "$rom" in *"$ROM_NAME"*) ;; *) echo "ВНИМАНИЕ: набор проверен на $ROM_NAME; с другой версией MIUI что-то может отличаться" ;; esac

	say "1/3 прошивка GPU (Adreno 640) из vendor.img: /vendor/firmware/a6*"
	rm -rf "$t/v"
	"$z" e -y -o"$t/v" "$img/vendor.img" firmware/a630_sqe.fw firmware/a640_gmu.bin 'firmware/a640_zap.*' >/dev/null
	mkdir -p "$R/fw-gpu"
	for f in a630_sqe.fw a640_gmu.bin a640_zap.mdt a640_zap.b00 a640_zap.b01 a640_zap.b02; do
		[ -f "$t/v/$f" ] || die "в vendor.img нет firmware/$f"
		cp "$t/v/$f" "$R/fw-gpu/"
	done
	ls "$R/fw-gpu" | sed 's/^/   /'

	say "2/3 прошивка аудио-DSP из NON-HLOS.bin: image/adsp.*"
	rm -rf "$t/n"
	# (7-Zip says "Unexpected end of archive" about this FAT image and returns 2, having taken every file out: what counts
	# is the files below, each checked)
	"$z" e -y -o"$t/n" "$img/NON-HLOS.bin" 'image/adsp.*' image/adspr.jsn image/adspua.jsn >/dev/null 2>&1 || true
	for f in adsp.mdt adsp.b00 adsp.b13 adspr.jsn adspua.jsn; do [ -s "$t/n/$f" ] || die "в NON-HLOS.bin нет image/$f"; done
	mkdir -p "$R/fw-audio"
	cp "$t/n"/adsp.* "$t/n"/adsp*.jsn "$R/fw-audio/"
	echo "   $(ls "$R/fw-audio" | wc -l | tr -d ' ') файлов"

	say "3/3 дерево устройств для загрузчика телефона: те же, что приложены к ядру в boot.img"
	python3 - "$img/boot.img" "$R/devices/cepheus/prebuilt/device-tree.dtb" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:8] != b'ANDROID!':
    sys.exit('boot.img: not an Android boot image')
ks, page = struct.unpack_from('<I', d, 8)[0], struct.unpack_from('<I', d, 36)[0]
k = d[page:page + ks]
# the kernel is followed by the device trees of every SoC revision; ABL takes the one that is this phone's
at, out = k.find(b'\xd0\x0d\xfe\xed'), []
while at >= 0:
    total = struct.unpack_from('>I', k, at + 4)[0]
    blob = k[at:at + total]
    if total > 4096 and len(blob) == total and b'qcom,msm-id' in blob and b'SM8150' in blob:
        out.append(blob)
        at = k.find(b'\xd0\x0d\xfe\xed', at + total)
    else:
        at = k.find(b'\xd0\x0d\xfe\xed', at + 4)
if not out:
    sys.exit('boot.img: no SM8150 device tree after the kernel')
open(sys.argv[2], 'wb').write(b''.join(out))
print(f'   деревьев: {len(out)} (SM8150 v1/v2, SM8150P v1/v2), {sum(map(len, out))} байт')
EOF
	rm -rf "$t"
	say "готово. Это прошивка ВАШЕЙ копии MIUI: в интернет её не выкладывайте. Дальше: ./setup.sh build IPSW"
}

# --- the images, on the Mac --------------------------------------------------------------------------------------
cmd_build() {
	local ipsw=${1:-} share="" metal=0
	[ -f "$ipsw" ] || die "укажите путь к $IPSW_NAME (его нужно скачать самостоятельно)"
	[ "$(size "$ipsw")" = $IPSW_SIZE ] || die "$ipsw: $(size "$ipsw") байт, а $IPSW_NAME — $IPSW_SIZE"
	[ -f "$R/fw-gpu/a640_gmu.bin" ] && [ -f "$R/fw-audio/adsp.mdt" ] && [ -f "$R/devices/cepheus/prebuilt/device-tree.dtb" ] ||
		die "сначала ./setup.sh firmware MIUI_ROM (прошивка GPU и DSP и дерево устройств из вашей MIUI)"
	mkdir -p "$W"
	say "1/6 файлы iOS из IPSW (sysimage/unpack.sh, около 10 минут, 8 ГБ на время работы)"
	(cd "$R" && sysimage/unpack.sh "$ipsw")
	say "2/6 загрузчик с идентичностью iPhone из вашего IPSW"
	(cd "$R" && xnuloader/mkidentity.sh sysimage/DeviceTree.d22ap.im4p && xnuloader/build.sh)
	say "3/6 конфигурация cepheus"
	(cd "$R" && python3 tools/hakios-config.py defconfig cepheus)
	say "4/6 шейдеры CoreAnimation для Metal (beta): из IPSW и QuartzCore этого Mac"
	rm -rf "$W/ca"
	if (cd "$R" && python3 tools/ca-metallib.py "$W/ca") && [ -f "$W/ca/QuartzCore-default.metallib" ]; then
		share=$W/ca metal=1
	else
		echo "   не получилось: сценарий metal будет недоступен (gl и basic работают)"
	fi
	say "5/6 образы загрузки: из RAM (system.apfs, около 2,5 ГБ) и для флеша (flashboot.apfs)"
	local common="PREBUILT=1 REST=1 GL=1 ADSP=1 METAL_PLUGIN=ca"
	(cd "$R/userland" && env $common EXTRA_SHARE="$share" ./build.sh && cp root.apfs root.ram.apfs)
	(cd "$R/userland" && env $common EXTRA_SHARE="$share" CRYPTEXES=flash FLASHBOOT=1 ./build.sh)
	echo $metal > "$W/metal"
	ls -la "$R/userland/root.ram.apfs" "$R/userland/system.apfs" "$R/userland/root.apfs" "$R/userland/flashboot.apfs"
	if [ -n "${RAM_ONLY:-}" ]; then
		say "готово (RAM_ONLY=1: без darwin.img). Запуск: ./run.sh gl ram"
		return
	fi
	say "6/6 darwin.img для флеша: корневой том iOS и оба криптекса"
	if [ -f "$W/darwin.img" ] && [ "$(size "$W/darwin.img")" = $DARWIN_IMG_SIZE ] && [ -f "$W/darwin.img.sha256" ]; then
		echo "  уже есть: $W/darwin.img"
	else
		python3 "$R/sysimage/ipsw-member.py" "$ipsw" 096-04088-077.dmg "$W/rootfs.dmg"
		python3 "$R/sysimage/ipsw-member.py" "$ipsw" 096-04666-078.dmg "$W/cryptex-os.dmg"
		python3 "$R/sysimage/ipsw-member.py" "$ipsw" 096-03536-083.dmg "$W/cryptex-app.dmg"
		local z found=""
		z=$(sevenzip)
		rm -rf "$W/parts"
		"$z" x -tdmg -y -o"$W/parts" "$W/rootfs.dmg" >/dev/null
		for f in "$W"/parts/*; do
			[ "$(dd if="$f" bs=1 skip=32 count=4 2>/dev/null)" = NXSB ] && found=$f
		done
		[ -n "$found" ] || die "в rootfs.dmg не нашлось APFS-контейнера"
		cat "$found" "$W/cryptex-os.dmg" "$W/cryptex-app.dmg" > "$W/darwin.img.part"
		rm -rf "$W/parts" "$W/rootfs.dmg" "$W/cryptex-os.dmg" "$W/cryptex-app.dmg"
		[ "$(size "$W/darwin.img.part")" = $DARWIN_IMG_SIZE ] || die "darwin.img не того размера"
		mv "$W/darwin.img.part" "$W/darwin.img"
		sha "$W/darwin.img" > "$W/darwin.img.sha256"
	fi
	if [ "$(cat "$W/darwin.img.sha256")" = $DARWIN_IMG_SHA ]; then
		echo "  sha256 $(cat "$W/darwin.img.sha256"): такой же, как у проверенного образа"
	else
		echo "  ВНИМАНИЕ: sha256 $(cat "$W/darwin.img.sha256") не тот, что у проверенного ($DARWIN_IMG_SHA)"
	fi
	say "готово. Без разметки можно сразу попробовать: ./run.sh gl ram. Для флеша: ./setup.sh backup TWRP.img"
}

# --- the phone -----------------------------------------------------------------------------------------------------
cmd_backup() {
	boot_twrp "${1:-}" backup
	local out
	out=$KIT/backups/cepheus-$(date +%Y%m%d-%H%M)
	say "копия только для чтения: $out (сотни МБ; на телефон ничего не пишется)"
	ADB=$(command -v adb) python3 "$R/tools/backup-partitions.py" "$out/main" --yes --with-bootchain --adb "$KIT/tools/adb-twrp.sh"
	ADB=$(command -v adb) python3 "$R/tools/backup-partitions.py" "$out/more" --yes \
		--only boot,recovery,dtbo,vbmeta,persistbak,keystore,frp,bluetooth --adb "$KIT/tools/adb-twrp.sh"
	say "готово: $out. СКОПИРУЙТЕ эту папку ещё куда-нибудь (в ней persist, IMEI и калибровки этого телефона;"
	echo "   в интернет её не выкладывайте). Дальше: ./setup.sh install new (или userdata); телефон остаётся в TWRP"
}

# write DIR/gpt-head.new and DIR/gpt-tail.new to LUN 0, only the blocks that differ, each copy's table before its header,
# then read both back. The phone is in TWRP.
write_gpt() {
	local dir=$1 blocks=$2
	adb shell 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true'
	adb push "$dir/gpt-head.new" /tmp/gpt-head.new >/dev/null
	adb push "$dir/gpt-tail.new" /tmp/gpt-tail.new >/dev/null
	python3 - "$dir" "$blocks" > "$dir/write.sh" <<'EOF'
import sys
d, disk, bs = sys.argv[1], int(sys.argv[2]), 4096
cmds = []
for name, base, header in (("head", 0, 1), ("tail", disk - 8, 7)):
    old, new = open(f"{d}/gpt-{name}.bin", "rb").read(), open(f"{d}/gpt-{name}.new", "rb").read()
    changed = [i for i in range(8) if old[i * bs:(i + 1) * bs] != new[i * bs:(i + 1) * bs]]
    for i in sorted(changed, key=lambda i: i == header):
        cmds.append(f"dd if=/tmp/gpt-{name}.new of=/dev/block/sda bs={bs} skip={i} seek={base + i} count=1 conv=notrunc,fsync")
print(" && ".join(cmds + ["sync"]))
EOF
	cat "$dir/write.sh"
	adb shell "$(cat "$dir/write.sh")" || die "запись не удалась. Не перезагружайте телефон и сохраните этот вывод"
	adb shell 'echo 3 > /proc/sys/vm/drop_caches' || true
	[ "$(lun0_sha head "$blocks")" = "$(sha "$dir/gpt-head.new")" ] || die "НАЧАЛО НЕ СОВПАЛО. Не перезагружайте телефон"
	[ "$(lun0_sha tail "$blocks")" = "$(sha "$dir/gpt-tail.new")" ] || die "КОНЕЦ НЕ СОВПАЛ. Не перезагружайте телефон"
	echo "   прочитано назад: обе копии таблицы как задумано"
}

cmd_install() {
	local where=${1:-} b dir blocks
	case "$where" in
	new|userdata) ;;
	system|vendor)
		die "в $where iOS не помещается, и загрузчик HakiOS его не примет:
  - $where на этом телефоне — $( [ "$where" = system ] && echo '3,5 ГиБ' || echo '1,5 ГиБ' ) на LUN 4 (sde), а iOS — 8,4 ГБ (корневой том Apple
    5,6 ГБ и криптексы), без места под /private/var;
  - загрузчик и ядро читают и пишут только раздел darwin на LUN 0 размером от 20 ГиБ
    (xnuloader/loader.c, block_is_darwin) — это защита остальных разделов телефона.
Выберите new (новый раздел из конца userdata, Android остаётся) или userdata (весь userdata под iOS)." ;;
	*) die "куда ставить: ./setup.sh install new|userdata [TWRP.img]" ;;
	esac
	[ -f "$W/darwin.img" ] || die "нет $W/darwin.img: сначала ./setup.sh build IPSW (без RAM_ONLY)"
	in_twrp "${2:-}" "install $where"
	if adb shell 'test -e /dev/block/by-name/darwin'; then
		say "раздел darwin уже есть ($(adb shell readlink /dev/block/by-name/darwin | tr -d '\r')): разметка не нужна"
		to_fastboot
		cmd_flash
		return
	fi
	b=$(last_backup)
	[ -n "$b" ] && [ -f "$b/gpt-sda-head.bin" ] || die "нет резервной копии: сначала ./setup.sh backup TWRP.img"
	blocks=$(lun0_blocks)
	say "1. таблица разделов на телефоне та же, что в копии $b?"
	[ "$(lun0_sha head "$blocks")" = "$(sha "$b/gpt-sda-head.bin")" ] || die "начало sda отличается от копии: сделайте ./setup.sh backup заново"
	[ "$(lun0_sha tail "$blocks")" = "$(sha "$b/gpt-sda-tail.bin")" ] || die "конец sda отличается от копии: сделайте ./setup.sh backup заново"
	echo "   да ($blocks блоков по 4096 в LUN 0)"
	dir=$(dirname "$b")/install-$where
	rm -rf "$dir"; mkdir -p "$dir"
	cp "$b/gpt-sda-head.bin" "$dir/gpt-head.bin"
	cp "$b/gpt-sda-tail.bin" "$dir/gpt-tail.bin"
	if [ "$where" = new ]; then
		say "2. новая таблица: darwin = последние $DARWIN_GIB ГиБ userdata (tools/userdata-carve.py проверяет обе копии GPT)"
		python3 "$R/tools/userdata-carve.py" --wipe "$dir" "$DARWIN_GIB" | tee "$dir/table.txt"
		confirm "Сейчас в таблицу разделов LUN 0 будут записаны изменённые блоки GPT, затем userdata будет стёрт
(fastboot -w): ВСЕ ДАННЫЕ ANDROID (фото, приложения, аккаунты) ПРОПАДУТ. Android останется и запустится как новый.
Остальные разделы не трогаются. Вернуть всё: ./setup.sh uninstall (копия таблицы — $b)." СТЕРЕТЬ
	else
		say "2. новая таблица: весь userdata становится darwin (tools/gpt-take.py; меняется только эта запись)"
		python3 "$KIT/tools/gpt-take.py" "$dir" userdata | tee "$dir/table.txt"
		confirm "Сейчас раздел userdata будет переименован в darwin: ВСЕ ДАННЫЕ ANDROID ПРОПАДУТ, и Android
БОЛЬШЕ НЕ ЗАПУСТИТСЯ (ему негде хранить данные), пока вы не сделаете ./setup.sh uninstall.
Остальные разделы не трогаются. Копия таблицы — $b." ОТДАТЬ
	fi
	say "3. запись: только блоки, которые отличаются, сначала записи разделов, потом заголовки"
	write_gpt "$dir" "$blocks"
	say "4. в fastboot"
	to_fastboot
	fastboot getvar partition-size:darwin 2>&1 | head -1
	[ "$where" = new ] && fastboot -w
	cmd_flash
}

cmd_flash() {
	fastboot_phone
	local hex blocks free p
	hex=$(fastboot getvar partition-size:darwin 2>&1 | awk '/partition-size:darwin:/ { print $2 }')
	[ -n "$hex" ] || die "загрузчик телефона не знает раздела darwin: сначала ./setup.sh install new|userdata"
	blocks=$(( hex / 4096 ))
	[ $blocks -ge $DARWIN_MIN_BLOCKS ] || die "darwin: $blocks блоков, нужно не меньше $DARWIN_MIN_BLOCKS"
	[ -f "$W/darwin.img" ] || die "нет $W/darwin.img: сначала ./setup.sh build IPSW"
	echo "darwin: $hex байт ($blocks блоков)"
	confirm "В раздел darwin (и только в него) будут записаны iOS (8,4 ГБ, около 5 минут) и пустой том
для /private/var. Если /private/var там уже был, он будет стёрт." ПРОШИТЬ
	free=$(df -Pk "$W" | awk 'NR == 2 { print int($4 / 1048576) }')
	[ "$free" -ge 9 ] || die "для кусков образа нужно 8 ГБ на диске, свободно $free"
	say "1. darwin.img кусками (fastboot для macOS не читает 8-гигабайтный файл целиком)"
	rm -rf "$W/pieces"
	python3 "$R/tools/sparse-split.py" "$W/darwin.img" "$W/pieces" \
		"$(( $(fastboot getvar max-download-size 2>&1 | awk '/max-download-size:/ { print $2 }') ))"
	for p in "$W"/pieces/piece-*.simg; do
		echo "### $(basename "$p")"
		fastboot flash darwin "$p"
	done
	rm -rf "$W/pieces"
	say "2. пустой том данных (HFS+, от блока 2042368 до конца darwin)"
	python3 "$R/tools/mkdatavol.py" "$W/data.simg" "$(mkfs_hfs)" "$blocks"
	fastboot flash darwin "$W/data.simg"
	say "готово. Проверить запись: ./setup.sh verify TWRP.img. Запустить iOS: ./run.sh gl"
}

cmd_verify() {
	boot_twrp "${1:-}" verify
	say "чтение darwin назад (около минуты)"
	local got
	got=$(adb exec-out 'dd if=/dev/block/by-name/darwin bs=4096 count=2042368 2>/dev/null | sha256sum' | cut -d' ' -f1)
	echo "  на телефоне: $got"
	echo "  darwin.img:  $(cat "$W/darwin.img.sha256")"
	[ "$got" = "$(cat "$W/darwin.img.sha256")" ] || die "НЕ СОВПАДАЕТ: повторите ./setup.sh flash"
	adb reboot bootloader
	say "совпадает. Телефон уходит в fastboot; дальше: ./run.sh gl"
}

cmd_uninstall() {
	in_twrp "${1:-}" uninstall
	local b blocks
	b=$(last_backup)
	[ -n "$b" ] && [ -f "$b/gpt-sda-head.bin" ] && [ -f "$b/gpt-sda-tail.bin" ] || die "нет резервной копии в $KIT/backups"
	blocks=$(lun0_blocks)
	confirm "Таблица разделов LUN 0 будет возвращена из $b: раздел darwin (iOS и /private/var) исчезнет,
userdata снова займёт его место и будет стёрт (fastboot -w)." ВЕРНУТЬ
	adb shell 'umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true'
	adb push "$b/gpt-sda-head.bin" /tmp/gpt-head.bin >/dev/null
	adb push "$b/gpt-sda-tail.bin" /tmp/gpt-tail.bin >/dev/null
	adb shell "dd if=/tmp/gpt-tail.bin of=/dev/block/sda bs=4096 seek=$((blocks - 8)) count=8 conv=notrunc,fsync && \
		dd if=/tmp/gpt-head.bin of=/dev/block/sda bs=4096 count=8 conv=notrunc,fsync && sync" || die "запись не удалась"
	adb shell 'echo 3 > /proc/sys/vm/drop_caches' || true
	[ "$(lun0_sha head "$blocks")" = "$(sha "$b/gpt-sda-head.bin")" ] || die "НАЧАЛО НЕ СОВПАЛО. Не перезагружайте телефон"
	[ "$(lun0_sha tail "$blocks")" = "$(sha "$b/gpt-sda-tail.bin")" ] || die "КОНЕЦ НЕ СОВПАЛ. Не перезагружайте телефон"
	to_fastboot
	fastboot -w
	say "таблица как до установки; fastboot reboot — и Android с чистой памятью"
}

case "${1:-}" in
check) cmd_check ;;
firmware) cmd_firmware "${2:-}" ;;
build) cmd_build "${2:-}" ;;
backup) cmd_backup "${2:-}" ;;
install) cmd_install "${2:-}" "${3:-}" ;;
flash) cmd_flash ;;
verify) cmd_verify "${2:-}" ;;
uninstall) cmd_uninstall "${2:-}" ;;
*) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 1 ;;
esac
