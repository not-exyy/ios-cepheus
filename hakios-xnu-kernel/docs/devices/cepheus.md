<!-- Made by tools/docs-gen.py from devices/cepheus/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Xiaomi Mi 9 (`cepheus`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sm8150` |
| Configuration | `make cepheus_defconfig` |
| Maintainer | local port |
| Repository | [`devices/cepheus/`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main/devices/cepheus) |

## What works

| Part | State | |
|---|---|---|
| display | works | UEFI framebuffer at 0x9c000000; XNU MiniIOMobileFramebuffer reaches 1080x2340 |
| usb | partly | U-Boot fastboot and loader USB staging work; resident xl run still hangs before returning |
| cpu | partly | XNU reaches Darwin kernel and framebuffer on SM8150; timer/GIC DT handoff is being validated |

The state is what has been seen on this device, written down in `devices/cepheus/device.conf`; a part the table does not name has not been looked at.
