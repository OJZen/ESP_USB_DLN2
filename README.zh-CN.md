# ESP USB DLN2

[English](README.md) | 简体中文

ESP USB DLN2 可以把 ESP32-S3 开发板模拟成一个 Linux 兼容的 DLN-2 风格
USB I/O 适配器。刷入固件并安装可选的主机支持包后，Linux 可以直接通过主线
内核已有驱动使用这块 S3，不需要额外编写专用 Linux 主机驱动。

当前目标开发板：WeAct ESP32S3-A。当前 Linux 兼容目标：主线内核中的
`dln2`、`gpio-dln2`、`i2c-dln2`、`spi-dln2` 和 `dln2-adc`。

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
. /home/jzen/.espressif/v5.5.4/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

刷入后，如果 Linux 没有立即绑定 DLN-2 设备，可以重新插拔 S3 native USB。

## 主机一次性安装

Linux 会自动创建 GPIO、I2C、SPI master 和 IIO ADC 子设备。但 Linux 不会为
一个裸 SPI controller 自动创建通用 `/dev/spidev*` 节点，而且默认权限通常
只允许 root 使用。安装一次 host support：

```bash
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

理想输出应全部为 `OK`，包括 `IIO buffer sysfs is writable`。安装细节和
排错见 `HOST_SUPPORT.md`。

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

## 测试夹具

`c3_test` 是 ESP32-C3 测试固件，用作可控的外部测试夹具。当前支持：

- GPIO 闭环验证。
- 用作 S3 SPI master 测试对象的 SPI slave fixture。
- 地址为 `0x52` 的 I2C slave fixture。

接线和命令说明见 `c3_test/README.md`。GPIO 对 GPIO 测试应在两块板之间
串联电阻。I2C 的 SDA/SCL 需要 3.3 V 上拉。

推荐的回归测试接线是固定 no-rewire C3/S3 harness v1：I2C 使用 C3
GPIO0/GPIO1 连接 S3 GPIO38/GPIO47，SPI 使用 C3 GPIO4-GPIO7 连接 S3
GPIO10-GPIO13，GPIO smoke/IRQ 验证使用 C3 channel 2 和 7 连接 S3 DLN
line 0 和 1。使用这套固定接线时，GPIO 验证命令是：

```bash
./scripts/c3_closed_loop_test.py --c3-port /dev/ttyACM2 --lines "0 1" --channels "2 7"
```

标准完整回归入口是：

```bash
./scripts/full_regression.sh
```

它会运行 host 就绪诊断、固定治具 GPIO/I2C/SPI 冒烟测试，以及 ADC IIO 自动
检查。加上 `--extended` 可以同时跑 I2C 压力/错误恢复和 SPI
mode/direction 矩阵：

```bash
./scripts/full_regression.sh --extended
```

日志会写入 `test_logs/`。ADC 已知电压电气测试仍然是手工项，因为当前 C3
fixture 不能产生经过校准的模拟电压。

较底层的固定治具 GPIO/I2C/SPI 回归入口是：

```bash
./scripts/c3_fixed_harness_regression.sh
```

完整 8 路 GPIO-only sweep 仍然保留，但它使用另一套 legacy GPIO-only 接线，
不要和固定 I2C/SPI harness 混接。

## 注意事项

- 固件故意使用 VID/PID `a257:2013` 来绑定 Linux 主线 DLN-2 驱动。不要在
  产品中使用这个 VID/PID。
- 所有 GPIO/I2C/SPI/ADC 信号都只能是 3.3 V。不要连接 5 V 信号。
- 不要把直接 GPIO loopback 当成常规验证方式。错误的输出配置可能让两个
  push-pull 输出互相短接。优先使用带串联电阻的 C3 fixture。
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
- 固定 C3 no-rewire harness 的常规 GPIO 闭环 smoke 覆盖只包含 line 0 和
  line 1。更广的 GPIO 覆盖需要 legacy GPIO-only 接线，或者后续扩展更大的
  外部 fixture。
- 当前只实现了一个 I2C port、一个带单个 chip select 的 SPI port，以及一个
  4 通道 ADC port。
- 未实现的 DLN-2 命令会明确返回错误，这是当前预期行为。

## 文档索引

- `README.md`：英文项目入口。
- `HOST_SUPPORT.md`：host 安装、udev/systemd 和 SPI spidev helper 细节。
- `DLN2_IMPLEMENTATION_PLAN.md`：从 Linux 驱动反推的命令覆盖计划和模块
  状态。
- `TECH_NOTES.md`：协议细节、引脚决策、验证记录和 bring-up 过程中遇到的坑。
- `PROGRESS.md`：按时间记录的里程碑。
- `c3_test/README.md`：ESP32-C3 fixture 固件使用说明。
