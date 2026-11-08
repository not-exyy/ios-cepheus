[Русский](README.md) | [English](README.en.md) | **简体中文**

# 适用于小米 9（cepheus）的 HakiOS：在你自己的手机上运行 iOS 16.5

本套件在小米 9 上运行 iOS 16.5 的用户空间（锁屏、主屏幕、设置、应用），底层是 **HakiOS**——一个开源的 XNU
内核，带有这款手机的驱动。内核、引导程序和 U-Boot 每次都**加载到内存（RAM）中**（`fastboot boot`）：手机的
bootloader、boot、recovery、modem 以及其他系统分区永远不会被刷写。

运行 iOS 有两种方式：

| | **从内存运行**（`ram`） | **从闪存运行**（`darwin` 分区） |
|---|---|---|
| 需要对手机做什么 | 什么都不用做 | 一次性在闪存上给 iOS 分出空间：新建一个分区，或使用整个 `userdata` |
| Android | 不受影响 | Android 数据会被清除（详见下文） |
| iOS 的设置和数据是否保留 | 否，退出后全部丢失 | 是，`/private/var` 位于闪存上 |
| 启动 | 每次运行都要通过 USB 传输 2.5 GB，约需一分钟 | 通过 USB 传输 15 MB，更快 |
| iOS 中的可用内存 | 几乎没有：2.4 GB 内存被镜像占用 | 足够 |
| 可用功能 | **试用模式**：显示、触摸、按键、GPU（basic 和 gl 模式）。**没有声音和振动**：系统内存不足，会不断把音频服务换出（振动也经过该服务）。**metal 无法启动** | 全部：三种模式及所有驱动 |

## 运行模式

| 模式 | 命令 | 可用功能 |
|---|---|---|
| **1. gl** —— 完整，OpenGL ES 渲染 | `./run.sh gl` | 所有驱动：触摸、按键、Adreno 640 GPU、声音（底部扬声器）、振动马达、电池电量、CPU 频率、温度传感器。CoreAnimation 通过 OpenGL ES（Mesa）在 GPU 上绘制——**推荐** |
| **2. metal** —— 完整，Metal 渲染 **（beta）** | `./run.sh metal` | 驱动与 gl 相同，但 CoreAnimation 通过 Metal 绘制。**Beta：** 打开应用比 gl 慢，部分元素可能绘制不正确 |
| **3. basic** —— 无驱动，仅显示画面 | `./run.sh basic` | 只有屏幕画面，由 CPU 绘制（约 10 帧/秒）。没有触摸、按键、声音、GPU 和电池。锁屏几秒后自动解锁。适合检查 iOS 能否在你的手机上启动 |

basic 和 gl 模式可以加上 `ram`，这样一切都从内存运行：`./run.sh basic ram`、`./run.sh gl ram`
（没有声音和振动，见上表）。`metal` 只能从闪存运行。不加 `ram` 时 iOS 从闪存启动，因此需要先完成安装的第 5 步。

在从闪存运行的 basic 模式中，闪存驱动仍然是启用的：没有它就无法从 `darwin` 分区读取 iOS。

## 哪些可用，哪些不可用

| 可用（gl 和 metal 模式） | 不可用 |
|---|---|
| 锁屏、主屏幕、应用、设置、Spotlight | 蓝牙、蜂窝网络（SIM 卡、通话、移动数据） |
| 触摸屏、手势、电源键和音量键 | 相机、麦克风、听筒 |
| 左侧的 AI 键作为“响铃/静音”开关 | 登录 Apple ID 和 iCloud（这款手机没有 Secure Enclave） |
| GPU：滚动时约 60 帧/秒（gl） | 在 HakiOS 引导程序中手机**不充电** |
| 底部扬声器发声、振动 | |
| 状态栏中的电池电量（只读） | |
| 屏幕亮度：控制中心和设置中的滑块 | 自动亮度（环境光传感器） |
| **Wi-Fi**（仅从闪存启动）：在“设置 → Wi-Fi”中选择网络，Safari 和应用可以上网 | Wi-Fi：WPA3 网络、无密码的开放网络、个人热点 |

### 从内存和从闪存：各自哪些可用

于 2026 年 11 月 6 日在小米 9 上测试。

| | 从闪存（`./run.sh gl`、`./run.sh metal`） | 从内存（`./run.sh gl ram`） |
|---|---|---|
| 锁屏、主屏幕、应用、设置 | ✅ | ✅（应用可能会自行关闭：系统内存不足） |
| 触摸屏、手势、按键、AI 键 | ✅ | ✅ |
| GPU、OpenGL ES 渲染（gl 模式） | ✅ | ✅ |
| Metal 渲染（metal 模式，beta） | ✅ | ❌ 窗口服务器崩溃：Metal 着色器不在内存镜像中，`./run.sh metal ram` 会拒绝启动 |
| 扬声器发声 | ✅ | ❌ 系统内存不足，会不断把音频服务换出 |
| 振动 | ✅ | ❌ 原因相同：iOS 通过音频服务发送振动 |
| 状态栏中的电池电量 | ✅ | ✅ |
| Wi-Fi | ✅（用 `nowifi` 关闭：`./run.sh gl nowifi`） | ❌ 调制解调器和 Wi-Fi 固件不在 RAM 镜像中 |
| CPU 频率、温度传感器 | ✅ | ✅ |
| 设置、照片、应用数据是否保留 | ✅（`/private/var` 位于闪存上） | ❌ 退出后全部丢失 |
| 是否需要改动手机闪存 | 是，一次（`./setup.sh install`） | 否 |

basic 模式（仅显示画面）从内存和从闪存运行效果相同。

原因：在内存模式下，iOS 镜像（2.4 GB）存放在手机内存中，留给 iOS 本身的只有约 140 MB。
从闪存运行时，iOS 直接从 `darwin` 分区读取，内存足够做所有事情。因此内存模式是一种不改动手机就能试用 iOS
的方式，而声音、振动和 Metal 需要从闪存运行。

## 准备工作

- **小米 9（cepheus）**，**已解锁 bootloader**（Mi Unlock）。本套件在国际版 MIUI 12.5
  **V12.5.1.0.RFAMIXM**（Android 11）、64 GB 型号上测试过。使用其他固件的引导程序（ABL）时可能无法工作。
- 相同版本的 **MIUI 线刷包（fastboot ROM）**：`cepheus_global_images_V12.5.1.0.RFAMIXM_…_global.tgz`
  （来自小米官网或镜像站）。套件从中提取 GPU、音频 DSP、调制解调器和 Wi-Fi 固件以及手机的设备树。它不会被安装到手机上。
- **iPhone X 的 iOS 16.5 IPSW**：`iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`，6,165,698,198 字节。
  需要你自己下载：套件中不含任何 Apple 文件。引导程序和 iOS 镜像会在你的 Mac 上由它构建。
- **cepheus 的 TWRP**（`.img`，例如来自 twrp.me）。仅在从闪存运行时需要：用于备份和分区。
  它被加载到内存中，不会被刷写。
- **Mac**（Apple Silicon 或 Intel）。在 Linux 上几乎所有功能也都能用（需要 `mkfs.hfsplus`），metal 模式除外，
  但只在 macOS 上测试过。
- 软件：Xcode Command Line Tools（`xcode-select --install`）、[Homebrew](https://brew.sh)，以及
  ```bash
  brew install sevenzip lld python
  ```
  ```bash
  brew install --cask android-platform-tools
  ```
  （也可以直接把 adb 和 fastboot 解压到本 README 旁边的 `platform-tools/` 文件夹中）。
- 构建期间需要**约 30 GB 可用空间**；仅用于内存模式约需 15 GB。

一次性检查所有条件：
```bash
./setup.sh check
```

## 安装

**下载套件**（约 50 MB）：
```bash
git clone https://github.com/not-exyy/ios-cepheus.git hakios-mi9
cd hakios-mi9
```
或者从 [Releases](https://github.com/not-exyy/ios-cepheus/releases) 页面下载压缩包。以下所有命令都在这个文件夹中执行。

**让手机进入 fastboot：** 关机，然后按住 **音量减 + 电源键**，直到出现兔子图标。

### 1. 从 MIUI 提取手机固件（不需要手机）
```bash
./setup.sh firmware ~/Downloads/cepheus_global_images_V12.5.1.0.RFAMIXM_20210611.0000.00_11.0_global.tgz
```
也可以指定已经解压的固件文件夹。套件从 `vendor.img` 中提取 GPU 固件（`fw-gpu/`），从 `NON-HLOS.bin` 中提取
音频 DSP 固件（`fw-audio/`），从 `boot.img` 中提取设备树（`devices/cepheus/prebuilt/device-tree.dtb`）。还从 `NON-HLOS.bin`
中提取调制解调器和 Wi-Fi 固件（`fw-modem/`，87 MB）：Wi-Fi 射频（WCN3998）由调制解调器的处理器控制，因此 HakiOS 会启动
调制解调器，但**不会让它连接蜂窝网络**：没有 SIM 卡、通话和移动数据，也不会写入调制解调器的分区（`modem`、EFS、`persist`）。
如果你以前做过这一步，请再做一次，然后重新做第 2 步，否则不会有 Wi-Fi。

### 2. 在 Mac 上构建（不需要手机）
```bash
./setup.sh build ~/Downloads/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
```
这需要 20–40 分钟。会得到引导程序、用于内存和闪存的镜像（`hakios-xnu-kernel/userland/`）以及
`work/darwin.img`（8.4 GB；其 sha256 会与经过验证的值比对）。如果你只打算从内存运行，可以跳过 darwin.img：
```bash
RAM_ONLY=1 ./setup.sh build ~/Downloads/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
```
对于 metal 模式，构建会从 IPSW 和你 Mac 上的 QuartzCore 生成 CoreAnimation 着色器。如果这台 Mac 上没有它们，
构建会如实提示：metal 将不可用，而 gl 和 basic 可以正常工作。

### 3. 首次从内存运行（不会擦除任何内容）
```bash
./run.sh basic ram
```
然后
```bash
./run.sh gl ram
```
约 2.5 GB 会传入手机内存（约一分钟），1–2 分钟后出现锁屏，然后是主屏幕。
从内存运行时没有声音和振动，metal 也无法启动：这些需要从闪存运行（第 4–5 步）。

### 4. 备份（只读；从闪存运行时需要）
```bash
./setup.sh backup ~/Downloads/twrp-3.7.1_12-0-cepheus.img
```
手机处于 fastboot → TWRP 被加载到内存 → 分区表和不可替代分区（persist、modemst、fsg 等）的副本保存到
`backups/cepheus-日期/`，每份副本都会被校验。**请把这个文件夹再复制到别处**：其中包含你这部手机特有的 IMEI
和校准数据。**不要把它上传到互联网。**

### 5. 将 iOS 安装到闪存：选择分区

第 4 步之后手机仍处于 TWRP 中。如果它已经重启，请让它进入 fastboot，并在命令末尾加上 TWRP 的路径。

| 安装位置 | 命令 | Android 会怎样 |
|---|---|---|
| **新分区** `darwin`（推荐） | `./setup.sh install new` | 从 `userdata` 末尾切出 32 GiB 给 iOS。Android 数据会被清除（`fastboot -w`），但 **Android 仍然保留**，会以全新状态启动，剩余约 19 GiB。修改大小的方法：`DARWIN_GIB=40 ./setup.sh install new`（不少于 20） |
| **整个 `userdata`** | `./setup.sh install userdata` | 整个 `userdata`（64 GB 型号上为 51 GiB）交给 iOS：该分区被重命名为 `darwin`，边界不变。Android 数据丢失，并且在你执行 `./setup.sh uninstall` 之前 **Android 无法再启动** |
| `system` 或 `vendor` | — | **不可能**。`system` 为 3.5 GiB，`vendor` 为 1.5 GiB，都位于 LUN 4 上，而 iOS 本身有 8.4 GB，还需要 `/private/var` 的空间。HakiOS 的引导程序和内核只读写 LUN 0 上大小不小于 20 GiB 的 `darwin` 分区：以此保护手机的其他分区。`./setup.sh install system` 会给出同样的解释，并且不做任何操作 |

`install` 会做以下事情：
1. 将手机上的分区表与备份进行比对；
2. 显示新的分区表。只有 `userdata` 和 `darwin` 两个条目会改变，GPT 的两份副本都会用 CRC 校验；
3. **要求确认**：`new` 需要输入 `СТЕРЕТЬ`（“擦除”），`userdata` 需要输入 `ОТДАТЬ`（“交出”）；
4. 只写入 GPT 中被修改的块，并读回校验；
5. 让手机进入 fastboot（对 `new` 还会执行 `fastboot -w`）；
6. 将 iOS 写入 `darwin` 分区，且只写入该分区：镜像的 11 个分块（约 5 分钟）以及用于 `/private/var` 的空卷。
   确认方式：输入 `ПРОШИТЬ`（“刷写”）。

确认词必须按原样以西里尔字母输入：脚本的提示信息是俄语的。

校验写入结果（可选）：
```bash
./setup.sh verify ~/Downloads/twrp-3.7.1_12-0-cepheus.img
```

## 运行

手机处于 fastboot。然后选择模式：

```bash
./run.sh gl
```
```bash
./run.sh metal
```
```bash
./run.sh basic
```

屏幕上先出现引导程序的绿色文字，1–2 分钟后出现锁屏：向上滑动或等待几秒。iOS 的运行没有时间限制。

**Wi-Fi**（gl 和 metal，从闪存启动）：在“设置 → Wi-Fi”中选择网络并输入密码，和 iPhone 上一样。支持 WPA2 网络；在 2.4 GHz
（iPhone 的个人热点）上测试过：下行约 65 Mbit/s，上行约 3 Mbit/s。5 GHz 和普通路由器尚未测试。暂不支持 WPA3 和无密码的网络。
手机有自己固定的 MAC 地址（由芯片序列号计算得出）。网络断开后，网络恢复时手机会自动重新连接。不使用 Wi-Fi：`./run.sh gl nowifi`。

- **退出到 HakiOS 引导程序：** 用 **5 根手指按住屏幕 1.5 秒**（gl、metal），或在另一个终端窗口中执行
  `./hakios-xnu-kernel/xl stop`（在 basic 中只能这样）。
- **不重启、从引导程序回到 iOS：** `./run.sh gl again`（把 gl 换成之前使用的模式，`ram` 也与之前保持一致）。`again` 之后没有 Wi-Fi：
  调制解调器不会第二次启动。需要 Wi-Fi 时请重启手机：按住电源键 15 秒，然后按音量减 + 电源键（fastboot），再执行 `./run.sh gl`。
- **让 iOS 在指定时间后自动退出：** `XL_SECS=3600 ./run.sh gl`（一小时）。
- **回到 Android：** 长按 **电源键**，或执行 `fastboot reboot`。在 `install new` 之后，Android 首次启动时会
  重新进行初始设置。在 `install userdata` 之后，执行 `./setup.sh uninstall` 之前 Android 无法启动。

## 删除 iOS，将手机恢复原状

手机处于 fastboot：
```bash
./setup.sh uninstall ~/Downloads/twrp-3.7.1_12-0-cepheus.img
```
分区表会从备份中恢复并校验：`darwin` 分区消失，`userdata` 恢复完整并被擦除（确认方式：输入 `ВЕРНУТЬ`，
“恢复”）。然后执行 `fastboot reboot`，Android 会以清空的存储启动。

## 如果出了问题

- **手机卡在开机徽标或黑屏：** 按住电源键 10–15 秒。系统中没有刷写任何东西，所以手机会照常重启。
  要进入 fastboot，同时按住音量减。
- **从 MIUI 过来后触摸不工作：** 将手机完全关机，然后直接开机进入 fastboot（音量减 + 电源键），
  不要从 Android 重启进入。
- **`./run.sh` 提示 «телефон не в fastboot»（“手机不在 fastboot 模式”）：** 检查数据线以及
  `fastboot devices` 的输出。
- **日志：** `hakios-xnu-kernel/logs/`，最新的是 `logs/last.log`。要在屏幕上显示详细的启动文字，使用参数
  `-v`，例如从内存运行时：`XL_CMDLINE="bootcpu=7 cpus=8 big_only=1 -v" ./run.sh basic ram`。`run.sh`
  会在启动前打印每种模式的参数字符串。
- **清除 `/private/var`**（iOS 设置、应用数据）：`./setup.sh flash`（手机处于 fastboot）。
- **每次运行时**，HakiOS 引导程序都会把自己的日志（约 4 KB）写入服务分区 `logdump`。这是 `darwin` 分区之外
  唯一的写入。

## 目录内容

```
README.md             本文件（俄语）
README.en.md          本文件（英语）
README.zh-CN.md       本文件（简体中文）
setup.sh              安装：check、firmware、build、backup、install、flash、verify、uninstall
run.sh                运行：gl、metal、basic 模式；ram；again
tools/gpt-take.py     install userdata：分区表中的 userdata 条目变为 darwin，别无其他改动
tools/adb-twrp.sh     用于从 TWRP 备份的 adb
tools/pack.sh         生成可分享的套件压缩包（见下文）
hakios-xnu-kernel/    HakiOS 源代码（cepheus 分支）
  prebuilt/             预编译文件：带小米 9 驱动的 XNU 内核、HakiOS 程序、Mesa、Metal 插件；SHA256SUMS
  devices/cepheus/      小米 9 移植：配置、U-Boot（prebuilt/u-boot.bin 及其源文件位于 u-boot/）
  xl                    通过 USB 控制手机上的引导程序
  docs/                 文档，docs/boot-args.md 说明每个内核参数的含义
work/                 构建时生成：darwin.img、metal 用的着色器
backups/              第 4 步时生成——个人数据，不要上传
```

## 如何分享套件

构建完成后，这个文件夹中包含 iOS 文件、小米固件和你的备份，因此**不要上传整个文件夹**。请制作压缩包：
```bash
tools/pack.sh
```
文件夹旁边会生成 `hakios-mi9.tar.gz` 及其 sha256。压缩包中只包含脚本、HakiOS 源代码和预编译的开源二进制文件。
脚本会检查压缩包，如果其中混入了构建产物、固件或备份中的任何内容，就会拒绝生成。如果你用 git 修改套件，
`.gitignore` 已经会阻止这些文件进入提交。

## 许可证与第三方文件

- 内核是 Apple XNU（`xnu-8796.121.2`，APSL 2.0），带有 HakiOS 的修改；源代码位于 `hakios-xnu-kernel/`
  （`xnu/hakios.patch`、`xnu/files/`、`drivers/`）。HakiOS 的引导程序、工具和程序采用 MIT 许可证。Mesa 采用 MIT。
  U-Boot 采用 GPL-2.0，源代码为上游 U-Boot v2026.07 和 `devices/cepheus/u-boot/files/`。详情见
  `hakios-xnu-kernel/LICENSE*`、`NOTICE`、`THIRD_PARTY_LICENSES.md`、`COMPLIANCE.md`。
- **套件中不含任何 Apple 和小米的文件。** iOS 来自你的 IPSW，GPU 和 DSP 固件以及设备树来自你的 MIUI 副本；
  这些都在你的电脑上构建。构建后不要上传 `work/`、`hakios-xnu-kernel/fw-*`、`hakios-xnu-kernel/sysimage/`、
  `hakios-xnu-kernel/userland/*.apfs`、`hakios-xnu-kernel/xnuloader/identity.h` 和 `backups/`。
- 这是一个研究项目。这里的 iOS 并不会让手机在 Apple 服务面前变成 iPhone。你对手机所做的一切，风险自负。
