<!-- Made by tools/docs-gen.py from devices/*/device.conf and socs/*/soc.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Devices

Every device with a repository, and how far each part of it works. A device of yours: [Port a device](../porting/index.md).

| Device | SoC | display | touch | buttons | cpu | gpu | storage | audio | battery | haptic | camera | torch | wifi | bluetooth | cellular | network | thermal | usb |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| [Xiaomi Mi 9](cepheus.md) | `sm8150` | yes |  |  | partly |  |  |  |  |  |  |  |  |  |  |  |  | partly |
| [QEMU virt (aarch64)](qemu.md) | `none` | yes | yes | yes |  | no | yes | yes |  |  |  |  |  |  |  | yes |  |  |
| [SM7325 phone](sm7325.md) | `sc7280` | yes | yes | yes | yes | yes | yes | yes | partly | yes | yes | partly | yes | no | partly |  | partly |  |

An empty cell is a part nobody has looked at on that device yet.

## SoC families

| SoC | What | |
|---|---|---|
| `sc7280` | Qualcomm SC7280 family (SM7325 "Snapdragon 778G", QCM6490; "yupik", "kodiak") | carried by sm7325 (`devices/sm7325/soc/`) |
| `sm8150` | Qualcomm Snapdragon 855 (SM8150) | carried by cepheus (`devices/cepheus/soc/`) |
