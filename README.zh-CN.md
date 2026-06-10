# ESP USB DLN2

[English](README.md) | 简体中文

ESP USB DLN2 可以把 ESP32-S3 开发板模拟成一个 Linux 兼容的 DLN-2 风格
USB I/O 适配器。刷入固件并安装可选的主机支持包后，Linux 可以直接通过主线
内核已有驱动使用这块 S3，不需要额外编写专用 Linux 主机驱动。

当前目标开发板：WeAct ESP32S3-A。当前 Linux 兼容目标：主线内核中的
`dln2`、`gpio-dln2`、`i2c-dln2`、`spi-dln2` 和 `dln2-adc`。

## 快速开始

下面的路径和 TTY 名称只是示例。请把 `IDF_PATH` 和 S3 烧录串口替换成你
自己机器上的实际值。

```bash
git clone https://github.com/Gdszzy/ESP_USB_DLN2.git
cd ESP_USB_DLN2

export IDF_PATH=/path/to/esp-idf
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash
```

刷入后，连接或重新插拔 S3 native USB，然后安装 Linux host support：

```bash
getent group plugdev || sudo groupadd -r plugdev
sudo ./scripts/install_host_support.sh
sudo usermod -aG plugdev "$USER"
newgrp plugdev
esp-usb-dln2-check
```

当 `esp-usb-dln2-check` 全部显示 `OK` 时，Linux 侧 GPIO/I2C/SPI/ADC
基础访问已经可用。日常使用不需要额外的开发板。

## 当前状态

已在当前开发主机验证：

- USB 以 VID/PID `a257:2013` 枚举，Linux 会自动绑定主线 `dln2` MFD
  驱动。
- GPIO 通过 `gpio-dln2` 暴露为 GPIO chardev，已验证读写和 IRQ 事件。
- I2C 通过 `i2c-dln2` 暴露为 Linux I2C adapter，已验证 raw read/write、
  两消息 `I2C_RDWR` 寄存器式读取、常见 SMBus ioctl 路径，以及短周期错误
  恢复压力测试。
- SPI 通过 `spi-dln2` 暴露为 Linux SPI master，并通过主机侧 helper 创建
  实验用 `spidev` 子设备以支持用户态传输。
- ADC 通过 `dln2-adc` 暴露为 IIO 设备，已验证 raw read、buffered read
  和 4 通道已知电压电气冒烟测试。
- 一次性 host support 安装后，当前主机上的普通用户可以直接访问
  GPIO/I2C/SPI/IIO。

这个项目仍处于实验室 bring-up 阶段，不是完成品。接外部硬件前请先阅读
`已知问题` 和 `注意事项`。

## 前置条件

硬件：

- WeAct ESP32S3-A，或 native USB 已接到主机的兼容 ESP32-S3 开发板。
- 一路 USB 用于烧录/日志，另一路 native USB 用于 DLN-2 模拟设备。

软件：

- ESP-IDF v5.5.4 是当前验证过的 SDK 版本。其它 ESP-IDF v5.x 版本可能可用，
  但不是当前验证目标。
- Linux 需要有主线 DLN-2 驱动：`dln2`、`gpio-dln2`、`i2c-dln2`、
  `spi-dln2` 和 `dln2-adc`。
- 主机构建和设备工具：`git`、Python 3、`make`、C 编译器、当前内核对应的
  kernel headers、`kmod`、`systemd` 和 `udev`。
- 推荐用户态工具：`libgpiod` 命令行工具、`i2c-tools` 和 `dkms`。
- 可选硬件仪表盘演示：Python `gpiod` binding 和 `Pillow`。

## 已实现的设备形态

- USB 2.0 full-speed vendor interface。
- Bulk OUT endpoint `0x01`，bulk IN endpoint `0x81`。
- Manufacturer string `Diolan`，product string `DLN-2 USB GPIO`，serial
  `12345678`。
- 基于 TinyUSB vendor endpoint 的 DLN-2 请求分帧和协议解析。
- Linux MFD 探测所需的 DLN-2 control 命令。
- 当前 Linux 驱动实际使用的 DLN-2 GPIO、I2C、SPI、ADC 命令子集。
- GPIO IRQ 事件和 ADC triggered-buffer 事件共用的 USB event 路径。
- ESP32-S3 物理引脚 ownership 层，用于避免 GPIO/I2C/SPI/ADC 互相抢占
  引脚。

## 引脚映射

Linux 看到的是 DLN-2 GPIO line offset，不是 ESP32-S3 GPIO 编号。
下表中的 GPIO 引脚按 Linux line offset 顺序排列：offset 0 是 GPIO1，
offset 1 是 GPIO2，后面依此类推。

| 功能 | Linux 名称 | ESP32-S3 引脚 |
| --- | --- | --- |
| GPIO lines | offsets 0-9 | GPIO1, GPIO2, GPIO4-GPIO9, GPIO18, GPIO21 |
| I2C port 0 | `/dev/i2c-X`, `dln2-i2c-*` | SDA GPIO38, SCL GPIO47 |
| SPI port 0 | `/sys/class/spi_master/spiX` | SCLK GPIO10, MOSI GPIO11, MISO GPIO12, CS0 GPIO13 |
| ADC port 0 | `/dev/iio:deviceX`, `dln2-adc` | GPIO14-GPIO17 |
| Native USB | DLN-2 USB device | GPIO19/GPIO20 |

保留或排除的引脚包括 ESP32-S3 strapping pins、native USB 引脚、UART0
烧录/日志引脚、PSRAM 引脚、JTAG 引脚和板载 LED 路径。完整排除列表见
`TECH_NOTES.md`。

## 构建和烧录

已验证环境为 ESP-IDF v5.5.4：

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

如果没有设置 `IDF_PATH`，请 source 你自己的 ESP-IDF checkout，例如
`. /path/to/esp-idf/export.sh`。刷入后，如果 Linux 没有立即绑定 DLN-2
设备，可以重新插拔 S3 native USB。

## 主机一次性安装

Linux 会自动创建 GPIO、I2C、SPI master 和 IIO ADC 子设备。但 Linux 不会为
一个裸 SPI controller 自动创建通用 `/dev/spidev*` 节点，而且默认权限通常
只允许 root 使用。安装一次 host support：

```bash
getent group plugdev || sudo groupadd -r plugdev
sudo ./scripts/install_host_support.sh
esp-usb-dln2-check
```

安装脚本会提供：

- DLN-2 GPIO、I2C、IIO 和 spidev 节点的 udev 权限规则。
- 处理 replug/re-enumeration 的 systemd oneshot。
- 一个小型主机侧 kernel helper，用于在 DLN-2 SPI controller 上注册临时
  `spidev` 子设备。当系统安装了 `dkms` 时，这个 helper 会作为 DKMS 包安装，
  内核升级后可以自动重建。
- `esp-usb-dln2-check` 就绪诊断命令。
- `esp-usb-dln2-uninstall` host support 卸载命令。

如果你想使用已有权限组，可以显式指定，例如
`sudo ./scripts/install_host_support.sh --group i2c`。如果当前用户不在选定
权限组里，加入该组并启动新的登录 shell：

```bash
sudo usermod -aG plugdev "$USER"
newgrp plugdev
esp-usb-dln2-check
```

理想输出应全部为 `OK`，包括 `IIO buffer sysfs is writable`。安装细节和
排错见 `HOST_SUPPORT.md`。

重启后不需要重复运行安装脚本。如果重新插拔或重启后 `/dev/spidev*` 没有出现，
先运行 `sudo systemctl start esp-usb-dln2-spi.service`，再运行
`esp-usb-dln2-check` 检查状态。

## 使用示例

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

使用 read-byte probing 扫描 DLN-2 I2C 总线：

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
cat /sys/bus/iio/devices/iio:device*/name
cat /sys/bus/iio/devices/iio:device*/in_voltage0_raw
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3"
```

## 可选硬件仪表盘

`scripts/oled_flash_button_dashboard.py` 是一个运行在 Linux 用户态的小演示。
它会驱动 SSD1306 128x64 I2C OLED，读取 W25Q 类 SPI NOR Flash 的基本信息，
并显示 4 个低电平有效按钮的状态。脚本只使用本项目暴露出来的 Linux
GPIO/I2C/SPI 接口，不需要额外的专用主机驱动。

所有外部硬件都只能接 3.3 V：

| 器件 | 信号 | ESP32-S3 引脚 / Linux line |
| --- | --- | --- |
| SSD1306 OLED | SDA | GPIO38 |
| SSD1306 OLED | SCL | GPIO47 |
| W25Q Flash | SCK | GPIO10 |
| W25Q Flash | DI / MOSI | GPIO11 |
| W25Q Flash | DO / MISO | GPIO12 |
| W25Q Flash | CS# | GPIO13 |
| W25Q Flash | WP#, HOLD# | 上拉到 3.3 V |
| 最上方按钮 | 低电平有效信号 | GPIO1 / line 0 |
| 第 2 个按钮 | 低电平有效信号 | GPIO2 / line 1 |
| 第 3 个按钮 | 低电平有效信号 | GPIO4 / line 2 |
| 最下方按钮 | 低电平有效信号 | GPIO5 / line 3 |

所有模块需要共地。每个按钮的信号点用约 10 kOhm 上拉到 3.3 V，按钮从信号点
接到 GND。bring-up 阶段建议在信号点和 S3 GPIO 之间串 1 kOhm 到 2.2 kOhm
电阻。OLED 模块如果没有自带 I2C 上拉，建议 SDA/SCL 各加约 4.7 kOhm 上拉到
3.3 V。

运行：

```bash
esp-usb-dln2-check
./scripts/oled_flash_button_dashboard.py --print-status
```

OLED 右侧从上到下显示 4 个按钮，按下时对应圆点会填充并放大。左侧显示
日期/时间、运行时间、SPI Flash 容量/ID/status、读写计数、按钮 bit/event
计数，以及预留的传感器占位符。Flash 探测默认只读 JEDEC ID 和 status，
不会擦除或写入芯片。

## 基础冒烟测试

安装 host support 后，下面这些检查不需要额外硬件：

```bash
esp-usb-dln2-check
gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3"
```

如果 GPIO line 没有连接外部硬件，也可以短暂驱动一个 line：

```bash
gpioset -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -t 100ms,0 0=1
```

I2C 和 SPI 的传输测试需要用户把实际 I2C/SPI 目标设备接到引脚映射表里的
S3 引脚上。

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

- SPI 用户态访问仍需要 host support 包，因为 Linux 不会为裸 SPI controller
  自动创建 `spidev`。
- 主机侧 SPI child registrar 仍是实验室 helper kernel module。有 DKMS 时
  可以覆盖常规内核升级；如果用 `--dkms no` 安装，升级内核后需要重新运行
  安装脚本。
- 当前 `I2C_RDWR` 验证覆盖的是 Linux `i2c-dln2` 的现有行为：它对每个
  `i2c_msg` 发送一个 DLN-2 transfer。严格要求 repeated START 的设备需要
  单独评估。
- ADC 4 个通道都已用已知输入电压完成电气冒烟测试。绝对精度和校准尚未
  验证。
- GPIO IRQ、I2C、SPI、ADC 的长时间耐久测试仍待补充。
- 无额外硬件的冒烟测试会验证枚举、权限、GPIO 元数据和 ADC IIO 路径。
  I2C/SPI 电气传输和 GPIO IRQ 需要外部目标硬件才能做端到端验证。
- 当前只实现了一个 I2C port、一个带单个 chip select 的 SPI port，以及一个
  4 通道 ADC port。
- 未实现的 DLN-2 命令会明确返回错误，这是当前预期行为。

## 反馈信息

如果要分享测试结果或反馈失败，请附上：

```bash
uname -a
lsusb -d a257:2013
esp-usb-dln2-check
journalctl -u esp-usb-dln2-spi.service -n 80 --no-pager
dmesg | grep -iE 'dln2|a257|spidev|iio'
```

## 许可证

主固件、host 脚本和文档使用 Apache License 2.0。详见 `LICENSE`。

`tools/spi_test_device/` 下的 Linux kernel helper 是 Linux 内核模块，按其
SPDX 头声明继续使用 GPL-2.0。

## 文档索引

- `README.md` / `README.zh-CN.md`：普通用户入口，覆盖烧录、host setup、
  功能状态、引脚映射、使用示例、安全注意事项和已知问题。
- `HOST_SUPPORT.md`：host 安装、udev/systemd 和 SPI spidev helper 细节。
- `TECH_NOTES.md`：协议细节、引脚决策、验证记录和 bring-up 过程中遇到的坑。
  主要面向开发者。
- `PROGRESS.md`：按时间记录的里程碑，属于历史记录，日常使用不需要阅读。
- `c3_test/README.zh-CN.md`：开发者专用 ESP32-C3 闭环测试夹具说明；日常使用
  S3 适配器不需要。
