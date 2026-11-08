[Русский](README.md) | **English** | [简体中文](README.zh-CN.md)

# HakiOS for the Xiaomi Mi 9 (cepheus): iOS 16.5 on your own phone

This kit runs the iOS 16.5 userland (lock screen, home screen, Settings, apps) on the Xiaomi Mi 9 on top of
**HakiOS**, an open XNU kernel with drivers for this phone. The kernel, the bootloader and U-Boot are loaded
**into RAM** every time (`fastboot boot`): the phone's bootloader, boot, recovery, modem and the other system
partitions are never flashed.

There are two ways to run iOS:

| | **From RAM** (`ram`) | **From flash** (the `darwin` partition) |
|---|---|---|
| what you have to do to the phone | nothing | give iOS space on the flash once: a new partition or the whole `userdata` |
| Android | left untouched | Android data is erased (details below) |
| are iOS settings and data kept | no, everything is lost when you exit | yes, `/private/var` on the flash |
| startup | about a minute to load 2.5 GB over USB on every run | 15 MB over USB, faster |
| free memory in iOS | almost none: 2.4 GB of RAM are taken by the image | enough |
| what works | **trial mode**: display, touch, buttons, GPU (basic and gl modes). **No sound and no vibration**: the system runs out of memory and keeps evicting the audio service (vibration also goes through it). **metal does not start** | everything: all three modes with all drivers |

## Run modes

| Mode | Command | What works |
|---|---|---|
| **1. gl** — full, OpenGL ES rendering | `./run.sh gl` | all drivers: touch, buttons, Adreno 640 GPU, sound (bottom speaker), vibration motor, battery level, CPU frequencies, temperature sensors. CoreAnimation draws on the GPU through OpenGL ES (Mesa) — **recommended** |
| **2. metal** — full, Metal rendering **(beta)** | `./run.sh metal` | all the same drivers, but CoreAnimation draws through Metal. **Beta:** apps open more slowly than in gl, some elements may be drawn incorrectly |
| **3. basic** — no drivers, display only | `./run.sh basic` | only the picture on the screen, drawn by the CPU (about 10 frames/s). No touch, buttons, sound, GPU or battery. The lock screen unlocks by itself after a few seconds. Handy for checking that iOS starts on your phone at all |

You can add `ram` to the basic and gl modes, and then everything runs from RAM: `./run.sh basic ram`, `./run.sh gl ram`
(no sound and no vibration, see the table above). `metal` works only from flash. Without `ram`, iOS starts from flash,
so you need installation step 5 first.

In the basic mode from flash the flash storage driver is still enabled: without it iOS cannot be read from the
`darwin` partition.

## What works and what does not

| Works (gl and metal modes) | Does not work |
|---|---|
| lock screen, home screen, apps, Settings, Spotlight | Bluetooth, cellular (SIM, calls, mobile data) |
| touchscreen, gestures, power and volume buttons | camera, microphone, earpiece |
| the AI button on the left is the Ring/Silent switch | Apple ID and iCloud sign-in (this phone has no Secure Enclave) |
| GPU: about 60 frames/s while scrolling (gl) | the phone **does not charge** in the HakiOS bootloader |
| sound through the bottom speaker, vibration | |
| battery level in the status bar (read only) | |
| screen brightness: the slider in Control Center and in Settings | auto-brightness (the ambient light sensor) |
| **Wi-Fi** (from the flash only): pick a network in Settings → Wi-Fi, internet in Safari and apps | Wi-Fi: WPA3 and open networks without a password, Personal Hotspot |

### From RAM and from flash: what works where

Tested on a Mi 9 on November 6, 2026.

| | From flash (`./run.sh gl`, `./run.sh metal`) | From RAM (`./run.sh gl ram`) |
|---|---|---|
| lock screen, home screen, apps, Settings | ✅ | ✅ (apps may close on their own: the system runs out of memory) |
| touchscreen, gestures, buttons, AI button | ✅ | ✅ |
| GPU, OpenGL ES rendering (gl mode) | ✅ | ✅ |
| Metal rendering (metal mode, beta) | ✅ | ❌ the window server crashes: the Metal shaders are not part of the RAM image, `./run.sh metal ram` refuses to start |
| sound through the speaker | ✅ | ❌ the system runs out of memory and keeps evicting the audio service |
| vibration | ✅ | ❌ for the same reason: iOS sends vibration through the audio service |
| battery level in the status bar | ✅ | ✅ |
| Wi-Fi | ✅ (turned off with the word `nowifi`: `./run.sh gl nowifi`) | ❌ the modem and Wi-Fi firmware is not in the RAM image |
| CPU frequencies, temperature sensors | ✅ | ✅ |
| settings, photos and app data are kept | ✅ (`/private/var` on the flash) | ❌ everything is lost when you exit |
| does anything on the phone's flash have to change | yes, once (`./setup.sh install`) | no |

The basic mode (display only) works the same from RAM and from flash.

Why: in RAM mode the iOS image (2.4 GB) sits in the phone's RAM, which leaves iOS itself about 140 MB.
From flash, iOS is read directly from the `darwin` partition, and there is enough memory for everything. So RAM is a
way to try iOS without changing anything on the phone, while sound, vibration and Metal need a run from flash.

## Requirements

- **Xiaomi Mi 9 (cepheus)** with an **unlocked bootloader** (Mi Unlock). The kit is tested on global MIUI 12.5
  **V12.5.1.0.RFAMIXM** (Android 11), 64 GB model. It may not work with the bootloader (ABL) of other firmware.
- **MIUI fastboot ROM** of the same version: `cepheus_global_images_V12.5.1.0.RFAMIXM_…_global.tgz`
  (from the Xiaomi website or mirrors). The kit takes the GPU, audio DSP, modem and Wi-Fi firmware and the phone's device tree
  from it. It is not installed on the phone.
- **iOS 16.5 IPSW for the iPhone X**: `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`, 6,165,698,198 bytes.
  You have to download it yourself: the kit contains no Apple files. The bootloader and the iOS images are built
  from it on your Mac.
- **TWRP for cepheus** (`.img`, for example from twrp.me). Needed only for running from flash: for the backup and the
  partitioning. It is loaded into RAM and not flashed.
- **Mac** (Apple Silicon or Intel). Almost everything works on Linux too (you need `mkfs.hfsplus`), except the metal
  mode, but it is tested only on macOS.
- Software: Xcode Command Line Tools (`xcode-select --install`), [Homebrew](https://brew.sh) and
  ```bash
  brew install sevenzip lld python
  ```
  ```bash
  brew install --cask android-platform-tools
  ```
  (you can also just unpack adb and fastboot into a `platform-tools/` folder next to this README).
- **About 30 GB of free space** during the build; about 15 for RAM only.

Check everything at once:
```bash
./setup.sh check
```

## Installation

**Download the kit** (about 50 MB):
```bash
git clone https://github.com/not-exyy/ios-cepheus.git hakios-mi9
cd hakios-mi9
```
or the archive from the [Releases](https://github.com/not-exyy/ios-cepheus/releases) page. All commands below are run
from this folder.

**Put the phone into fastboot:** power it off, then hold **Volume down + Power** until the bunny appears.

### 1. Phone firmware from MIUI (no phone needed)
```bash
./setup.sh firmware ~/Downloads/cepheus_global_images_V12.5.1.0.RFAMIXM_20210611.0000.00_11.0_global.tgz
```
You can also pass an already unpacked firmware folder. The kit extracts the GPU firmware (`fw-gpu/`) from
`vendor.img`, the audio DSP firmware (`fw-audio/`) from `NON-HLOS.bin`, and the device tree
(`devices/cepheus/prebuilt/device-tree.dtb`) from `boot.img`. Also from `NON-HLOS.bin`: the modem and Wi-Fi firmware
(`fw-modem/`, 87 MB). The Wi-Fi radio (WCN3998) is driven by the modem's processor, so HakiOS starts the modem but **does not
connect it to the cellular network**: no SIM, no calls, no mobile data, and the modem partitions (`modem`, EFS, `persist`) are
not written. If you did this step before, do it again and then step 2 again, otherwise there will be no Wi-Fi.

### 2. Build on the Mac (no phone needed)
```bash
./setup.sh build ~/Downloads/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
```
This takes 20–40 minutes. You get the bootloader, the images for RAM and for flash (`hakios-xnu-kernel/userland/`) and
`work/darwin.img` (8.4 GB; its sha256 is checked against a verified one). If you will run only from RAM, you can
skip darwin.img:
```bash
RAM_ONLY=1 ./setup.sh build ~/Downloads/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
```
For the metal mode the build makes the CoreAnimation shaders from the IPSW and from your Mac's QuartzCore. If this Mac
does not have them, the build says so: metal will be unavailable, while gl and basic will work.

### 3. First run from RAM (erases nothing)
```bash
./run.sh basic ram
```
then
```bash
./run.sh gl ram
```
About 2.5 GB go into the phone's RAM (about a minute); after 1–2 minutes the lock screen appears, then the home screen.
From RAM there is no sound and no vibration, and metal does not start: they need a run from flash (steps 4–5).

### 4. Backup (read only; needed for running from flash)
```bash
./setup.sh backup ~/Downloads/twrp-3.7.1_12-0-cepheus.img
```
Phone in fastboot → TWRP is loaded into RAM → a copy of the partition tables and the irreplaceable partitions (persist,
modemst, fsg, …) goes to `backups/cepheus-DATE/`, and every copy is verified. **Copy this folder somewhere else as
well**: it holds the IMEI and calibrations of your particular phone. **Do not upload it to the internet.**

### 5. Installing iOS on the flash: choose a partition

The phone stays in TWRP after step 4. If it has already rebooted, put it into fastboot and add the path to TWRP at the
end of the command.

| Where | Command | What happens to Android |
|---|---|---|
| **a new partition** `darwin` (recommended) | `./setup.sh install new` | 32 GiB for iOS are cut from the end of `userdata`. Android data is erased (`fastboot -w`), but **Android stays** and starts as new, with the remaining ~19 GiB. To change the size: `DARWIN_GIB=40 ./setup.sh install new` (at least 20) |
| **the whole `userdata`** | `./setup.sh install userdata` | the whole `userdata` (51 GiB on the 64 GB model) is given to iOS: the partition is renamed to `darwin`, its boundaries do not change. Android data is lost, and **Android no longer starts** until you run `./setup.sh uninstall` |
| `system` or `vendor` | — | **impossible**. `system` is 3.5 GiB and `vendor` is 1.5 GiB, both on LUN 4, while iOS is 8.4 GB plus space for `/private/var`. The HakiOS bootloader and kernel read and write only the `darwin` partition on LUN 0, at least 20 GiB in size: this protects the rest of the phone's partitions. `./setup.sh install system` explains the same and does nothing |

What `install` does:
1. compares the partition table on the phone with the backup;
2. shows the new table. Only the `userdata` and `darwin` entries change, both copies of the GPT are checked by CRC;
3. **asks for confirmation**: for `new` you type `СТЕРЕТЬ` ("ERASE"), for `userdata` — `ОТДАТЬ` ("GIVE AWAY");
4. writes only the changed GPT blocks and reads them back;
5. puts the phone into fastboot (for `new` it also runs `fastboot -w`);
6. writes iOS to the `darwin` partition and only there: 11 chunks of the image (about 5 minutes) and an empty volume for
   `/private/var`. Confirmation: type `ПРОШИТЬ` ("FLASH").

The confirmation words must be typed exactly as shown, in Cyrillic: the scripts print their messages in Russian.

Verify the write (optional):
```bash
./setup.sh verify ~/Downloads/twrp-3.7.1_12-0-cepheus.img
```

## Running

Phone in fastboot. Then choose a mode:

```bash
./run.sh gl
```
```bash
./run.sh metal
```
```bash
./run.sh basic
```

First the bootloader's green text appears on the screen, after 1–2 minutes the lock screen: swipe up or wait a few
seconds. iOS runs with no time limit.

**Wi-Fi** (gl and metal from the flash): Settings → Wi-Fi, pick a network and type its password, as on an iPhone. WPA2 networks;
tested on 2.4 GHz (an iPhone's hotspot): about 65 Mbit/s down, about 3 Mbit/s up. 5 GHz and ordinary routers are not
tested yet. WPA3 and networks without a password are not supported yet. The phone has its own permanent MAC address (made
from the chip's serial number). If the network drops, the phone joins it again when it is back. Without Wi-Fi: `./run.sh gl nowifi`.

- **Exit to the HakiOS bootloader:** hold **5 fingers on the screen for 1.5 seconds** (gl, metal) or run
  `./hakios-xnu-kernel/xl stop` from another terminal window (in basic this is the only way).
- **Return to iOS from the bootloader without rebooting:** `./run.sh gl again` (instead of gl, the same mode as before,
  with or without the same `ram`). There is no Wi-Fi after `again`: the modem does not start a second time. For Wi-Fi, restart
  the phone: hold Power for 15 seconds, then Volume down + Power (fastboot), and `./run.sh gl`.
- **To make iOS exit by itself after a set time:** `XL_SECS=3600 ./run.sh gl` (one hour).
- **Return to Android:** hold **Power** for a long time or run `fastboot reboot`. After `install new`, Android goes
  through the initial setup again on its first start. After `install userdata`, Android will not start until you
  run `./setup.sh uninstall`.

## Remove iOS and restore the phone as it was

Phone in fastboot:
```bash
./setup.sh uninstall ~/Downloads/twrp-3.7.1_12-0-cepheus.img
```
The partition table is restored from the backup and verified: the `darwin` partition disappears, `userdata` is whole
again and is erased (confirmation: `ВЕРНУТЬ`, "RESTORE"). Then `fastboot reboot`, and Android starts with clean storage.

## If something goes wrong

- **The phone hangs on the logo or a black screen:** hold Power for 10–15 seconds. Nothing in the system has been
  flashed, so the phone reboots as usual. To get into fastboot, also hold Volume down.
- **Touch does not work after MIUI:** power the phone off completely and turn it on straight into fastboot
  (Volume down + Power), not by rebooting from Android.
- **`./run.sh` says «телефон не в fastboot» ("the phone is not in fastboot"):** check the cable and the output of
  `fastboot devices`.
- **Logs:** `hakios-xnu-kernel/logs/`, the latest is `logs/last.log`. For verbose boot text on the screen use the
  `-v` argument, for example from RAM: `XL_CMDLINE="bootcpu=7 cpus=8 big_only=1 -v" ./run.sh basic ram`. `run.sh`
  prints each mode's argument string before starting.
- **Erase `/private/var`** (iOS settings, app data): `./setup.sh flash` (phone in fastboot).
- **On every run** the HakiOS bootloader writes its log (about 4 KB) to the service partition `logdump`. This is the
  only write outside the `darwin` partition.

## What is inside

```
README.md             this file in Russian
README.en.md          this file in English
README.zh-CN.md       this file in Simplified Chinese
setup.sh              installation: check, firmware, build, backup, install, flash, verify, uninstall
run.sh                running: gl, metal, basic modes; ram; again
tools/gpt-take.py     install userdata: the userdata entry in the partition table becomes darwin, nothing else
tools/adb-twrp.sh     adb for the backup from TWRP
tools/pack.sh         a shareable archive of the kit (see below)
hakios-xnu-kernel/    HakiOS sources (cepheus branch)
  prebuilt/             ready-made: XNU kernel with Mi 9 drivers, HakiOS programs, Mesa, Metal plugin; SHA256SUMS
  devices/cepheus/      the Mi 9 port: configuration, U-Boot (prebuilt/u-boot.bin and its source files in u-boot/)
  xl                    controls the bootloader on the phone over USB
  docs/                 documentation, docs/boot-args.md explains every kernel argument
work/                 appears during the build: darwin.img, shaders for metal
backups/              appears at step 4 — PERSONAL, do not upload
```

## How to share the kit

After the build this folder contains iOS files, Xiaomi firmware and your backups, so **do not upload the whole
folder**. Make an archive:
```bash
tools/pack.sh
```
`hakios-mi9.tar.gz` and its sha256 appear next to the folder. The archive contains only the scripts, the HakiOS sources
and the ready-made open binaries. The script checks the archive and refuses to make it if anything from the build, the
firmware or the backups got in. If you edit the kit in git, `.gitignore` already keeps these files out of commits.

## Licenses and third-party files

- The kernel is Apple XNU (`xnu-8796.121.2`, APSL 2.0) with HakiOS changes; the source code is in `hakios-xnu-kernel/`
  (`xnu/hakios.patch`, `xnu/files/`, `drivers/`). The HakiOS bootloader, tools and programs are MIT. Mesa is MIT.
  U-Boot is GPL-2.0, the sources are upstream U-Boot v2026.07 and `devices/cepheus/u-boot/files/`. Details:
  `hakios-xnu-kernel/LICENSE*`, `NOTICE`, `THIRD_PARTY_LICENSES.md`, `COMPLIANCE.md`.
- **The kit contains no Apple or Xiaomi files.** iOS comes from your IPSW, the GPU and DSP firmware and the device tree
  from your copy of MIUI; all of this is built on your computer. Do not upload `work/`, `hakios-xnu-kernel/fw-*`,
  `hakios-xnu-kernel/sysimage/` after the build, `hakios-xnu-kernel/userland/*.apfs`,
  `hakios-xnu-kernel/xnuloader/identity.h` or `backups/`.
- This is a research project. iOS here does not turn the phone into an iPhone for Apple services. Whatever you do with
  your phone, you do at your own risk.
