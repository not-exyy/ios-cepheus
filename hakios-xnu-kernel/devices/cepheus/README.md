# Device: CEPHEUS (cepheus)

Made by `tools/new-device.py` on 2026-11-02, from the file cepheus.dtb. Edit it as the port goes.

| What | Value |
|---|---|
| Board (the tree's `model`) | CEPHEUS |
| Compatible | `qcom,sm8150-cdp qcom,sm8150 qcom,cdp` |

## The SoC addresses

**This tree differs from the reference phone's** in 4 value(s). The generic parser does not recognize the vendor names for `soc_cmd_db`, `soc_mpss_mem` and `modem_ipa_fw_at`; those values are recorded manually in `soc.args`.

| Argument | Value | Against the reference |
|---|---|---|
| `soc_ipa_base` | `0x1e40000` | the same as the reference |
| `soc_gsi_base` | `0x1e04000` | the same as the reference |
| `soc_ipcc` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_aoss` | `0xc300000` | the same as the reference |
| `soc_rsc` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_apps_smmu` | `0x15000000` | the same as the reference |
| `soc_cmd_db` | `0xc3f000c` | vendor `qcom,cmd-db` node; recorded manually |
| `soc_mpss_mem` | `0x8e000000` | vendor `modem_region`; recorded manually |
| `modem_ipa_fw_at` | `0x98f10000` | vendor `pil_ipa_gsi_region`; recorded manually |
| `soc_smem` | `0x86000000` | **differs from the reference** (`0x80900000`) |
| `soc_imem` | `0x14600000` | the same as the reference |
| `soc_ipa_stream` | `0x520` | **differs from the reference** (`0x480`) |
| `soc_ipa_stream_uc` | `0x522` | **differs from the reference** (`0x482`) |
| `soc_pas_mpss` | `0x4` | the same as the reference |
| `soc_pas_ipa` | `0xf` | the same as the reference |
| `soc_ipa_fw` | `ipa_fws` | **differs from the reference** (`yupik_ipa_fws`) |

`soc.args` is what `tools/soc-args.py` read out of the tree; `pull-soc.sh` makes it again from the phone on
adb. The run scripts add it to the kernel command line. Where each argument comes from in the tree:
`devices/redwood/README.md`.

## What is this phone's own

The kernel, loader and user space know nothing about a phone: addresses, interrupts, rails and firmware
come from its tree and its Android. What is written for one phone is the list below. The head of each file
says where its numbers came from; that is the map of what to rewrite.

- [ ] **UEFI**: the firmware that starts the loader: devices/redwood/uefi/ is a port of Mu-Silicium; the vendor's drivers are extracted from your own phone (uefi/README.md)
- [ ] **Display, buttons, touch**: drivers/input/hakios.input.c, hakios.buttons.c, hakios.goodix.c (the reference phone's controller; another phone has another)
- [ ] **Storage**: hakios.ufs.c and hakios.ufshc.c (UFS); a phone with eMMC needs another driver
- [ ] **CPU clocks**: hakios.cpufreq.c
- [ ] **GPU**: hakios.adreno.c (Adreno 642L here: another generation needs its own firmware and registers)
- [ ] **Wi-Fi**: hakios.wifi.c, hakios.lx.c and wifi/lx/ (the WCN6750 radio of the reference phone)
- [ ] **Audio and battery**: hakios.adsp.c, hakios.audio.c, hakios.sound.c, hakios.speaker.c, hakios.mic.c, hakios.battery.c
- [ ] **Cellular**: hakios.mpss.c, hakios.qmi.c, hakios.nas.c, hakios.ipa.c, hakios.cell.c; soc.args carries the addresses, the IPA layout is for IPA 4.11 (docs/cellular.md)
- [ ] **Haptics and cameras**: hakios.haptic.c, hakios.camera.c

## Configuration

The kernel's settings are layered the way an Android kernel's are: the common kernel, the SoC's, this phone's
(`docs/configs.md`). Two short files make this phone a configuration:

    configs/cepheus.config          CONFIG_DEVICE="cepheus"   and whatever is true of this phone only
    configs/cepheus_defconfig       #include common_defconfig
                                   #include <the SoC's fragment: sc7280.config for the SC7280 family>
                                   #include cepheus.config

Then `make cepheus_defconfig`. Turn on only what this phone has been shown to do; `tools/hakios-config.py check` says what the
choice needs.

The drivers are part of the configuration too (`docs/drivers-config.md`): every one is built unless a configuration leaves it out,
and the addresses, pins and rails in each are the reference phone's. Leave out in `configs/cepheus.config` what has not been
ported to this phone yet (`# CONFIG_HAKIOS_GOODIX is not set`, ...; `configs/qemu_defconfig` leaves out all of them), and put
each one back when it has been made to work here. `tools/hakios-config.py show` lists what is built in.

## Firmware

Pulled from your own phone, never committed: `devices/redwood/pull-firmware.sh`, or
`pull-firmware-root.sh` where the firmware is readable by root only. Check with `make doctor`.

## Before you open a pull request

`tools/provenance-check.py` must pass: no vendor firmware, no UEFI drivers, no Apple files in the tree.

## The repository

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the sm8150 family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

| File | What |
|---|---|
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/cepheus_defconfig` | the common kernel, the SoC's fragment, this device's: `make cepheus_defconfig` |
| `configs/cepheus.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer (sm8150): `soc.conf`, `configs/sm8150.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/cepheus.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check cepheus`.

## Verified SM8150 hardware facts

The phone's own DTB is saved in `prebuilt/device-tree.dtb` and was read from
`/sys/firmware/fdt`.

- framebuffer: `0x9c000000`, `1080x2340`
- GICv3 distributor: `0x17a00000`, redistributors: `0x17a60000`
- architected timer memory window: `0x17c20000`
- UFS host: `0x1d84000`
- touch controller: `st,fts` at I²C controller `0xc80000`, address `0x49`
- touch pins: I²C GPIO55/56, interrupt GPIO122, reset GPIO12

The loader and XNU kernel are verified to reach the Darwin kernel and bring up
the framebuffer from RAM. Touch is intentionally not enabled yet: the common
HakiOS driver targets the Redwood SPI controller, while cepheus uses the STM
FTS I²C protocol. The resident `xl run` path still needs a clean on-device
validation with the Qualcomm timer node now present in the generated XNU device
tree.
