#!/bin/bash
# HakiOS: start iOS 16.5 on the Xiaomi Mi 9. Nothing is flashed by this: U-Boot, the loader and the kernel go into RAM
# (fastboot boot), and after an ordinary reboot the phone is what it was.
#
#   ./run.sh SCENARIO [ram] [nowifi] [again]
#
#   SCENARIO   gl      every driver (touch, keys, GPU, sound, vibration, battery, CPU clocks, temperature), drawn by the
#                      GPU through OpenGL ES                                                         <- start with this
#              metal   every driver, drawn by the GPU through Metal (BETA: opening apps is slower than gl, some
#                      pictures may come out wrong)
#              basic   no drivers: only the picture, drawn by the processor; no touch, no keys, no sound
#   ram        iOS wholly from RAM (2.5 GB uploaded, about a minute): no partition needed, nothing is kept.
#              A TRIAL: the image takes 2.4 GB of the phone's memory and the system keeps killing programs for room,
#              the sound server among them, so from RAM there is no sound and no vibration, and metal is refused
#              (its shaders do not fit in that image either). basic and gl (picture, touch, keys, GPU) work.
#              Without it: iOS from the "darwin" partition (./setup.sh install), /private/var kept there: everything
#   nowifi     without Wi-Fi (gl and metal from the flash have it: the network is chosen in Settings > Wi-Fi). For Wi-Fi
#              the modem's processor is started, offline: the radio is driven by it; no SIM, no calls, no mobile data
#   again      the phone is still in the HakiOS loader from the run before and the image is already in its RAM: only start
#
#   XL_SECS=N        hand back to the loader after N seconds (default 0: never)
#   XL_CMDLINE="..." the kernel's arguments instead of the scenario's (./run.sh SCENARIO prints them)
#
# The phone has to be in fastboot (Volume down + Power with it switched off), or in the HakiOS loader from a run before.
# End a run: five fingers on the screen for 1.5 s (gl, metal), or ./hakios-xnu-kernel/xl stop from another terminal,
# or hold Power for 15 s. Logs: hakios-xnu-kernel/logs/ (the last one: logs/last.log)
set -euo pipefail
KIT=$(cd "$(dirname "$0")" && pwd)
R=$KIT/hakios-xnu-kernel
export PATH="$KIT/platform-tools:$PATH"
export DEVICE=cepheus XL_HOME=fastboot XL_FIRMWARE=u-boot

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 1; }
scenario= mode=flash again= nowifi=
for a in "$@"; do
	case $a in
	gl|metal|basic) scenario=$a ;;
	ram) mode=ram ;;
	nowifi) nowifi=1 ;;
	again) again=1 ;;
	*) usage ;;
	esac
done
[ -n "$scenario" ] || usage
command -v fastboot >/dev/null ||
	{ echo "не найден fastboot: brew install --cask android-platform-tools, или положите adb и fastboot в $KIT/platform-tools"; exit 1; }

# What each scenario asks of the kernel (docs/boot-args.md in hakios-xnu-kernel says what every word does).
#   bootcpu=7 cpus=8 big_only=1   the system on the four big cores, the three small ones not started (smoother)
#   cpufreq=1 thermal=1           the cores at full speed; the run stops at 105 °C
#   touchscreen=1 buttons=1       touch (ST FingerTip) and the keys; the AI key is the ring/silent switch
#   gpu=1                         the Adreno 640, for CoreAnimation's OpenGL ES renderer
#   haptic=5                      the vibration motor, for iOS
#   adsp=1 sound=4                the audio DSP and the bottom speaker
#   fuelgauge=2                   the battery's charge in the status bar (read only)
#   backlight=2                   the screen's brightness: the slider in Control Center and in Settings
#   enable_skstb sb_bind=0        SpringBoard's main thread on the fastest core
#   metal=1 ca_metallib=...       CoreAnimation through Metal instead of OpenGL ES (beta)
#   modem_* wcn_* wifi=1          Wi-Fi (the WCN3998 through the ath10k driver): the modem's processor started for the
#                                 radio and kept offline, the network joined from Settings > Wi-Fi (flash only)
full="bootcpu=7 cpus=8 big_only=1 cpufreq=1 thermal=1 touchscreen=1 buttons=1 gpu=1 haptic=5 adsp=1 sound=4 fuelgauge=2 backlight=2 enable_skstb sb_bind=0"
case $scenario in
gl) args=$full ;;
metal)
	[ "$(cat "$KIT/work/metal" 2>/dev/null)" = 1 ] ||
		{ echo "сценарий metal не собран: ./setup.sh build нужен macOS с QuartzCore default.metallib (см. ./setup.sh check)"; exit 1; }
	args="$full metal=1 ca_metallib=/hakios/share/ca/QuartzCore-default.metallib" ;;
basic) args="bootcpu=7 cpus=8 big_only=1" ;;
esac
wifi="modem_step=10 modem_new=11 modem_keep=1 modem_listen=3 wcn_lx=1 wcn_power=1 wifi_station=1 wifi=1"
if [ $scenario != basic ] && [ $mode = flash ] && [ -z "$nowifi" ]; then
	if [ -n "$again" ]; then
		# seen on the phone (2026-11-07): a second kernel from the same loader does not get the modem's memory back
		echo "== без Wi-Fi: после again модем второй раз не запускается; для Wi-Fi перезапустите телефон (Питание 15 с, затем fastboot)"
	elif [ "$(cat "$KIT/work/wifi" 2>/dev/null)" = 1 ]; then
		args="$args $wifi"
	else
		echo "== без Wi-Fi: flashboot.apfs собран без прошивки модема (./setup.sh firmware MIUI_ROM, затем ./setup.sh build IPSW)"
	fi
fi
if [ $mode = ram ]; then
	# seen on the phone (2026-11-06): from RAM, 203 programs killed for memory in 5 minutes, mediaserverd 18 times (no
	# sound, no vibration: iOS plays the vibration through it too); metal's backboardd aborts again and again
	# ("library not found": the framework shader libraries are left out of the RAM image, 176 MB)
	[ $scenario = metal ] && { echo "metal из RAM не работает (шейдеры Metal не помещаются в образ RAM): ./run.sh metal — с флеша"; exit 1; }
	[ $scenario = gl ] && echo "== из RAM: картинка, тач, кнопки и GPU работают; звука, вибро и Wi-Fi не будет (не хватает памяти), для них — с флеша"
fi

cd "$R"
[ -f xnuloader/loader.efi ] || { echo "нет xnuloader/loader.efi: сначала ./setup.sh build IPSW"; exit 1; }
[ -f devices/cepheus/prebuilt/device-tree.dtb ] || { echo "нет дерева устройств: сначала ./setup.sh firmware MIUI_ROM"; exit 1; }
if [ -z "$again" ]; then
	fastboot devices 2>/dev/null | grep -q fastboot ||
		{ echo "телефон не в fastboot (Громкость вниз + Питание на выключенном телефоне)"; exit 1; }
fi
rm -f /tmp/darwin-hakios-phone.ran /tmp/darwin-hakios-phone.run /tmp/darwin-hakios-phone.who

if [ $mode = flash ]; then
	[ -f userland/flashboot.apfs ] || { echo "нет userland/flashboot.apfs: сначала ./setup.sh build IPSW"; exit 1; }
	[ -n "$again" ] || ./xl ramdisk userland/flashboot.apfs
	cmdline="root_apfs=ufs_darwin ufs=2 ufs_write=1 var_hfs=ufs_darwin_data $args"
	ramdisk=userland/root.apfs
else
	[ -f userland/system.apfs ] && [ -f userland/root.ram.apfs ] || { echo "нет образов: сначала ./setup.sh build IPSW"; exit 1; }
	[ -n "$again" ] || ./xl ramdisk userland/system.apfs
	cmdline=$args
	ramdisk=userland/root.ram.apfs
fi
echo "== сценарий $scenario ($([ $mode = ram ] && echo 'из RAM' || echo 'с флеша, раздел darwin')): ${XL_CMDLINE:-$cmdline}"
XL_RAMDISK=$ramdisk XL_SECS=${XL_SECS:-0} XL_FOLLOW=5 XL_CMDLINE="${XL_CMDLINE:-$cmdline}" ./xl run
