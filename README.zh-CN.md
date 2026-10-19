# ESP USB DLN2

[English](README.md) | 简体中文

ESP USB DLN2 把 ESP32-S3 开发板变成 USB GPIO/I2C/SPI/ADC 适配器。
同一份固件在 Linux 上使用主线 DLN-2 驱动，在 Windows 10/11 上使用系统
WinUSB 和 Python 客户端。当前目标开发板是 **WeAct ESP32S3-A**。

| 主机平台 | 应用访问方式 | 从哪里开始 |
| --- | --- | --- |
| Linux | GPIO chardev、I2C、SPI/spidev、IIO ADC | 下方[快速开始](#快速开始)；[主机安装细节](HOST_SUPPORT.md) |
| Windows 10/11 | 通过 WinUSB 使用 Python 客户端，无第三方 Python 依赖 | [Windows 使用说明](WINDOWS.md) |

固件提供 10 路 GPIO、1 路 I2C、1 路带 CS0 的 SPI 和 4 路 ADC。
所有外部信号均为 **3.3 V**。项目仍处于实验验证阶段，接线前请阅读[已知问题](#已知问题)。

## 快速开始

1. 确认[前置条件](#前置条件)，然后[构建和烧录](#构建和烧录) S3 固件。
2. 连接 **native USB** 口，按自己的操作系统完成[主机一次性安装](#主机一次性安装)。
3. 查看[引脚映射](#引脚映射)，再运行[使用示例](#使用示例)。

已刷入当前固件的开发板可以直接从主机安装开始。ESP32-C3 只用于可选的开发者测试夹具。

## 前置条件

- WeAct ESP32S3-A，或与下方引脚映射一致的兼容 ESP32-S3 开发板。
- 用于开发板烧录/日志串口和 native USB 设备口的 USB 连接。串口用于烧录，
  native USB 口提供 DLN-2 设备。
- 用于构建固件的 Git 和 **ESP-IDF v5.5.4**。其它 ESP-IDF v5.x 版本尚未在本项目验证。

Linux 主机还需要：

- 主线驱动：`dln2`、`gpio-dln2`、`i2c-dln2`、`spi-dln2`、`dln2-adc`。
- host support 安装工具：`make`、C 编译器、当前内核的 headers、`kmod`、
  `systemd` 和 `udev`。安装 `dkms` 可让 SPI helper 在内核升级后自动重建。
- 使用和测试工具：Python 3、`libgpiod` 命令行工具、`i2c-tools`。
  下方 GPIO 命令采用 libgpiod v2 语法。

Windows 10/11 主机还需要：

- Python 3.10+；客户端只使用标准库和系统 WinUSB。
- 支持 WinUSB 自动绑定的当前固件。旧固件需要先更新一次，见 [Windows 使用说明](WINDOWS.md)。

## 构建和烧录

获取源码：

```shell
git clone https://github.com/Gdszzy/ESP_USB_DLN2.git
cd ESP_USB_DLN2
```

按自己的平台选择下面一组命令。请替换 SDK 路径和示例串口；这里烧录的是 S3，
不是 C3 测试夹具。

**Linux / Bash：**

```bash
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash
```

**Windows / 已配置 ESP-IDF v5.5.4 的 PowerShell：**

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash
```

刷入后，连接或重新插拔 S3 的 **native USB** 口。需要查看固件日志时，使用同一
烧录串口运行 `idf.py -p <FLASH_PORT> monitor`。

## 主机一次性安装

### Windows 10/11

新固件会自动绑定系统 WinUSB。在项目根目录运行：

```powershell
py scripts\dln2_windows.py devices
py scripts\dln2_windows.py check
```

`check` 只读取身份和接口能力，不驱动引脚。预期硬件 ID 为 `0x00000200`，
GPIO 为 10 路，ADC 为 4 路。I/O 命令、Python 调用和驱动检查见 [Windows 使用说明](WINDOWS.md)。

### Linux

Linux 会自动绑定 DLN-2 驱动。首次安装 host support，用于配置普通用户权限并创建
SPI 的 `spidev` 节点：

```bash
getent group plugdev || sudo groupadd -r plugdev
sudo ./scripts/install_host_support.sh
sudo usermod -aG plugdev "$USER"
newgrp plugdev
esp-usb-dln2-check
```

`newgrp` 会打开已取得新组权限的 shell。就绪检查应全部为 `OK`，包括
`IIO buffer sysfs is writable`。

正常重启和重新插拔 USB 不需要重复安装。权限组选择、DKMS、`spidev` 节点缺失
和卸载方法见 [HOST_SUPPORT.md](HOST_SUPPORT.md)。

## 引脚映射

两个平台的 GPIO 命令都使用 **DLN-2 line offset**，不是 ESP32-S3 GPIO 编号：
line 0 对应 GPIO1，line 1 对应 GPIO2，依此类推。ADC channel 0–3 按顺序对应 GPIO14–17。

| 功能 | 通道 | ESP32-S3 引脚 |
| --- | --- | --- |
| GPIO | line offset 0–9 | GPIO1, GPIO2, GPIO4–GPIO9, GPIO18, GPIO21 |
| I2C | port 0 | SDA GPIO38, SCL GPIO47 |
| SPI | port 0、CS0 | SCLK GPIO10, MOSI GPIO11, MISO GPIO12, CS GPIO13 |
| ADC | channel 0–3 | GPIO14–GPIO17 |
| Native USB | DLN-2 设备 | GPIO19/GPIO20 |

外设需要共地，信号电平为 3.3 V。启动绑带、native USB、UART0、PSRAM、JTAG
和板载 LED 所用引脚已保留或排除。完整列表见 [TECH_NOTES.md](TECH_NOTES.md)，
接线前请阅读[注意事项](#注意事项)。

## 使用示例

下列命令用于 **Linux**；Windows 的 I/O 命令见 [Windows 使用说明](WINDOWS.md#使用)。
执行输出命令前，请确认接线。

查找 GPIO chip：

```bash
gpiodetect
gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00
```

读取和驱动 GPIO line：

```bash
gpioget -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 --numeric 0 1 2
gpioset -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -t 100ms,0 0=1
gpiomon -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -e both 0
```

查找 I2C adapter：

```bash
for d in /sys/class/i2c-dev/i2c-*; do
    printf '%s: ' "$d"
    cat "$d/name"
done
```

将 `<BUS_NUMBER>` 替换为名称以 `dln2-i2c-` 开头的总线编号，
然后使用 read-byte probing 扫描：

```bash
i2cdetect -r -y <BUS_NUMBER>
```

通过 host support 创建的 `spidev` 节点使用 SPI：

```bash
esp-usb-dln2-check
ls -l /dev/spidev*
```

读取 ADC raw 值：

```bash
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3" --skip-buffer
```

## 应用示例

可选的 [OLED 仪表盘](examples/oled_flash_button_dashboard/README.md)使用 SSD1306
屏幕、W25Q 类 SPI Flash、AHT20/BMP280 类传感器和 4 个按钮，通过适配器的
Linux GPIO/I2C/SPI 接口工作。接线和运行方式见示例 README。

## 当前状态

Linux 一栏汇总此前的实机验证；Windows 一栏对应 2026-10-19 完成的 WinUSB 适配验证。

| 功能 | Linux 验证 | Windows 验证 |
| --- | --- | --- |
| USB | 自动绑定 `dln2` | 自动绑定系统 WinUSB、身份读取、重新枚举 |
| GPIO | 读写和 IRQ 事件 | 输入读取、输出锁存/方向、释放；未测输出电压 |
| I2C | raw 传输、两消息 `I2C_RDWR`、SMBus 路径、错误恢复 | AHT20 + BMP280 读数、AHT20 CRC 校验、NACK 恢复 |
| SPI | SPI master 和主机创建的 `spidev` 访问 | mode 0–3 和传输边界；未验证实际从设备数据 |
| ADC | raw/buffered 读取、4 通道已知电压冒烟测试 | raw 范围和异步事件；未校准精度 |

Windows 和 Ubuntu WSL 各通过 24 项主机协议测试。**加入 WinUSB 描述符后，尚未
重新做 Linux 实机 USB 绑定回归。** WSL 协议测试不覆盖 Linux 设备驱动。
详细结果见 [Windows 验证记录](WINDOWS.md#本次验证记录2026-10-19)。

## 开发指南

### 从哪里改代码

| 路径 | 职责 |
| --- | --- |
| [main/](main/) | USB 描述符、请求分帧、事件发送 |
| [components/dln2/](components/dln2/) | DLN-2 命令、GPIO/I2C/SPI/ADC 实现、引脚占用管理 |
| [scripts/dln2_windows.py](scripts/dln2_windows.py) | Windows 发现设备、WinUSB 传输、Python API、CLI |
| [host_support/](host_support/) / [tools/spi_test_device/](tools/spi_test_device/) | Linux 权限、插拔处理、SPI 子设备注册 |
| [tests/](tests/) | 主机协议测试、编译产物 USB 描述符检查、Windows 实机冒烟测试 |
| [c3_test/](c3_test/) | 可选的 ESP32-C3 GPIO/I2C/SPI 闭环夹具 |

USB 使用单个全速 vendor interface、Bulk OUT `0x01` / IN `0x81`、VID/PID
`a257:2013` 和序列号 `12345678`。GPIO IRQ 与 ADC 事件共用 USB 事件通道。
协议和实现决策见 [TECH_NOTES.md](TECH_NOTES.md)。

### 无硬件测试

在项目根目录运行：

```bash
python3 -B -m unittest discover -s tests -v
```

Windows 使用 `py -B -m unittest discover -s tests -v`。

### 基础冒烟测试

Linux 安装 host support 后，以下检查不需要连接外设：

```bash
esp-usb-dln2-check
gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3"
```

这些命令检查设备访问、GPIO 元数据和 ADC raw/buffered 路径，不验证测量精度。
I2C/SPI 传输和 GPIO IRQ 检查需要外部硬件。

编译产物描述符检查及 Windows 实机冒烟测试见 [Windows 开发检查](WINDOWS.md#开发检查)；
其中实机冒烟测试会驱动 GPIO，必须使用无外部接线的裸板。Linux 完整回归需要先按
[C3 测试夹具说明](c3_test/README.zh-CN.md)接线，再运行
[scripts/full_regression.sh](scripts/full_regression.sh)。

## 注意事项

- 固件故意使用 VID/PID `a257:2013` 来绑定 Linux 主线 DLN-2 驱动。不要在
  产品中使用这个 VID/PID。
- 所有 GPIO/I2C/SPI/ADC 信号都只能是 3.3 V。不要连接 5 V 信号。
- 不要把 GPIO 输出直接互连。错误的输出配置可能让两个 push-pull 输出互相
  短接。连接 GPIO 到其它硬件时请使用串联电阻或合适的外部保护。
- GPIO0、GPIO3、GPIO45、GPIO46 是 ESP32-S3 上的 strapping-sensitive
  引脚，本固件不把它们暴露为 GPIO line。
- GPIO19/GPIO20 是 native USB 引脚，必须保留给 DLN-2 USB 设备。
- 固件里为 I2C 启用了 S3 内部上拉，但实验室环境建议在 SDA/SCL 上加
  3.3 V 外部上拉，典型值约 4.7 kOhm。
- ADC 当前使用 ESP32-S3 ADC2。因为本固件不使用 Wi-Fi，所以这个选择目前
  可接受。如果后续加入 Wi-Fi，需要重新评估 ADC2。
- ADC raw 值是 10-bit 命令路径读数，不是经过校准的电压测量结果。

## 已知问题

- Linux `spidev` 访问依赖实验性的主机 helper 内核模块。DKMS 可处理常规内核
  升级；用 `--dkms no` 安装时，升级后需要重新运行安装脚本，见[主机安装说明](HOST_SUPPORT.md)。
- I2C 写和读是独立事务。Linux `i2c-dln2` 对每个 `i2c_msg` 发送一个 DLN-2
  transfer，Windows 客户端也分别执行写和读。要求 repeated START 的设备需要单独验证。
- ADC 绝对精度和校准尚未验证。
- GPIO IRQ、I2C、SPI、ADC 的长时间耐久测试仍待补充。
- 当前仅实现这些主机工具使用的 DLN-2 命令子集；未实现的命令会明确返回错误。

## 反馈信息

Windows 反馈请附系统/Python 版本，以及 `py scripts\dln2_windows.py devices`
和 `py scripts\dln2_windows.py check` 的输出。

Linux 反馈请附：

```bash
uname -a
lsusb -d a257:2013
esp-usb-dln2-check
journalctl -u esp-usb-dln2-spi.service -n 80 --no-pager
dmesg | grep -iE 'dln2|a257|spidev|iio'
```

## 文档索引

- [WINDOWS.md](WINDOWS.md)：Windows 安装、CLI/Python 使用、驱动检查和验证记录。
- [HOST_SUPPORT.md](HOST_SUPPORT.md)：Linux 安装、udev/systemd 和 SPI helper。
- [TECH_NOTES.md](TECH_NOTES.md)：协议细节、引脚决策和开发中遇到的问题。
- [OLED 仪表盘](examples/oled_flash_button_dashboard/README.md)：Linux 应用与接线示例。
- [C3 测试夹具](c3_test/README.zh-CN.md)：可选的硬件闭环回归。
- [PROGRESS.md](PROGRESS.md)：历史里程碑；当前使用方法以上述指南为准。

## 许可证

主固件、host 脚本和文档使用 Apache License 2.0。详见 [LICENSE](LICENSE)。

`tools/spi_test_device/` 下的 Linux kernel helper 是 Linux 内核模块，按其
SPDX 头声明继续使用 GPL-2.0。
