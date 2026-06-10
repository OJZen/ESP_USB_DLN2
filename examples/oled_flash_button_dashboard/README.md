# OLED Flash Button Dashboard Example

这个示例演示如何把 ESP USB DLN2 当作普通 Linux GPIO/I2C/SPI 适配器使用：
Linux 侧脚本通过 DLN-2 暴露出来的接口驱动 SSD1306 OLED、读取 W25Q 类 SPI
NOR Flash，并显示 4 个按钮的状态。

它是应用层示例，不是固件必需组件。普通功能验证仍以根目录 README 和
`esp-usb-dln2-check` 为准。

## 硬件

- ESP32-S3 已刷入本项目固件，native USB 已连接到 Linux 主机。
- SSD1306 128x64 I2C OLED，常见地址为 `0x3c`。
- W25Q128 或兼容 W25Q 类 SPI NOR Flash。
- 4 个按钮。
- 3.3 V 电源、共地、若干 1 kOhm 到 2.2 kOhm 串联电阻。
- 如果 OLED 模块没有自带 I2C 上拉，SDA/SCL 需要约 4.7 kOhm 上拉到 3.3 V。

所有信号都必须是 3.3 V。不要接 5 V TTL，也不要接 RS232 电平。

## 接线

OLED：

| OLED 信号 | ESP32-S3 |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SDA | GPIO38 |
| SCL | GPIO47 |

SPI Flash：

| Flash 信号 | ESP32-S3 |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SCK / CLK | GPIO10 |
| DI / MOSI | GPIO11 |
| DO / MISO | GPIO12 |
| CS# | GPIO13 |
| WP# | 3.3 V 上拉 |
| HOLD# | 3.3 V 上拉 |

按钮使用低电平有效。每个按钮的信号点用约 10 kOhm 上拉到 3.3 V，按钮从信号点
接到 GND。bring-up 阶段建议在信号点和 S3 GPIO 之间串 1 kOhm 到 2.2 kOhm。

| 屏幕位置 | Linux GPIO line | ESP32-S3 |
| --- | --- | --- |
| 最上方按钮 | line 0 | GPIO1 |
| 第 2 个按钮 | line 1 | GPIO2 |
| 第 3 个按钮 | line 2 | GPIO4 |
| 最下方按钮 | line 3 | GPIO5 |

## 主机准备

先确认 ESP USB DLN2 主机支持已经安装，并且当前设备可用：

```bash
esp-usb-dln2-check
```

输出应全部为 `OK`，特别是 GPIO、I2C、SPI spidev 和 IIO 权限项。

脚本需要 Python `gpiod` binding 和 Pillow：

```bash
python3 - <<'PY'
import gpiod
from PIL import Image
print("ok")
PY
```

如果缺依赖，请用当前发行版的包管理器安装对应包。Arch Linux 上通常是
`python-gpiod` 和 `python-pillow`。

## 运行

在仓库根目录运行：

```bash
./examples/oled_flash_button_dashboard/oled_flash_button_dashboard.py --print-status
```

常用参数：

```bash
./examples/oled_flash_button_dashboard/oled_flash_button_dashboard.py \
    --refresh-hz 8 \
    --spi-speed 100000 \
    --print-status
```

如果只想测试 OLED 和按钮，临时跳过 SPI Flash：

```bash
./examples/oled_flash_button_dashboard/oled_flash_button_dashboard.py --no-flash
```

退出时按 `Ctrl-C`。脚本会清屏并释放 GPIO line。

## 屏幕内容

左侧字段：

- 日期和时间：来自 Linux 主机系统时间。
- `UP`：脚本已经运行的时间。
- `Flash`：根据 JEDEC ID 推算出的 Flash 容量。
- `ID` / `S`：Flash JEDEC ID 和 status register。
- `Events`：累计按钮按下事件数。
- `T` / `H`：预留给未来温湿度传感器的数据位置。

右侧从上到下是 4 个按钮的圆点：

- 空心圆表示未按下。
- 实心或放大的圆表示当前按下，或刚刚捕捉到一次按下事件。

控制台日志会打印每条 GPIO line 的 `up/down` 状态、累计事件数和 Flash 状态。

## 实现方式

OLED 控制采用整帧 framebuffer 刷新：

1. 在内存中生成一张 `128x64` 的 1-bit 图像。
2. 用 Pillow 在图像上绘制文字、线条和按钮圆点。
3. 把二维像素图转换成 SSD1306 的 page 格式。每 8 个纵向像素压成 1 个字节。
4. 通过 Linux `i2c-dev` 写入 SSD1306 GDDRAM。

脚本默认 `--refresh-hz 8`，即每秒发送 8 次完整 1024 字节显示数据。OLED 面板
本身的扫描刷新由 SSD1306 控制器负责。

SPI Flash 探测默认是只读的，只发送 JEDEC ID 和 status register 读取命令，
不会写入、擦除或修改芯片内容。

## 常见问题

`device busy: stop other dashboard/gpiod/spidev users and retry`

说明 GPIO line、I2C 或 SPI 设备正在被其它进程占用。先停止旧的 dashboard、
`gpiomon`、`gpioset` 或其它访问同一设备的进程后再运行。

`missing device: no dln2-i2c adapter found`

说明 S3 native USB 没有正确枚举为 DLN-2 设备，或 Linux 驱动没有绑定。先运行
`esp-usb-dln2-check`。

Flash ID 一直是异常值，例如 `FFFFFF` 或 `000000`

优先检查 SPI 接线、Flash 供电、GND 共地、WP#/HOLD# 是否上拉，以及 CS# 是否
接到 GPIO13。

按钮没有变化

确认按钮是低电平有效：信号点上拉到 3.3 V，按下时被按钮接到 GND。再确认
4 个按钮分别连接到 GPIO1、GPIO2、GPIO4、GPIO5。
