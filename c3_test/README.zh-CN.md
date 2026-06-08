# ESP32-C3 Test Fixture

[English](README.md) | 简体中文

`c3_test` 是开发者用的 ESP32-C3 闭环测试固件，用来验证 ESP32-S3
USB DLN2 固件的 GPIO、I2C 和 SPI 行为。普通用户只使用 S3 作为 USB I/O
适配器时，不需要刷这个固件，也不需要额外准备 C3。

## 用途

这个固件把 ESP32-C3 变成一个通过 USB-Serial/JTAG 控制的外部测试夹具：

- GPIO probe：由 Linux 控制 S3 GPIO，C3 读取；或 C3 输出，Linux 读取。
- GPIO IRQ smoke：C3 产生脉冲，Linux 通过 `gpiomon` 观察 S3 事件。
- I2C slave：C3 作为地址 `0x52` 的 I2C 从设备，S3/Linux 作为 master。
- SPI slave：C3 作为 SPI 从设备，S3/Linux 作为 master。

当前推荐使用固定 no-rewire harness v1。接线一次后，GPIO smoke、I2C 和 SPI
测试都可以通过软件切换完成，不需要反复改线。

## 默认通道

| C3 channel | ESP32-C3 GPIO |
| ---------- | ------------- |
| 0          | GPIO0         |
| 1          | GPIO1         |
| 2          | GPIO3         |
| 3          | GPIO4         |
| 4          | GPIO5         |
| 5          | GPIO6         |
| 6          | GPIO7         |
| 7          | GPIO10        |

这些 C3 引脚刻意避开了常见风险脚：GPIO2、GPIO8、GPIO9 这类启动绑带相关脚，
GPIO18/GPIO19 native USB，GPIO20/GPIO21 UART0，以及 flash 相关引脚。

## 固定 No-Rewire Harness v1

只连接 3.3 V 信号，不要连接 5 V。C3 GND 必须和 S3 GND 共地。如果两块板都
通过 USB 供电，不要把两边 5 V 轨直接接在一起。

GPIO/SPI 这类 push-pull 信号建议串联电阻，1 kOhm 到 4.7 kOhm 都可以，
2.2 kOhm 是当前实验室默认值。I2C SDA/SCL 需要 3.3 V 外部上拉，典型值
4.7 kOhm。

| 用途 | C3 信号 / channel | C3 GPIO | 保护 | S3 信号 / Linux line | S3 GPIO |
| ---- | ----------------- | ------- | ---- | -------------------- | ------- |
| I2C SDA | SDA | GPIO0 | 3.3 V 上拉 | SDA | GPIO38 |
| I2C SCL | SCL | GPIO1 | 3.3 V 上拉 | SCL | GPIO47 |
| SPI SCLK | SCLK | GPIO4 | 串联电阻 | SCLK | GPIO10 |
| SPI MOSI | MOSI | GPIO5 | 串联电阻 | MOSI | GPIO11 |
| SPI MISO | MISO | GPIO6 | 串联电阻 | MISO | GPIO12 |
| SPI CS0 | CS | GPIO7 | 串联电阻 | CS0 | GPIO13 |
| GPIO smoke A | channel 2 | GPIO3 | 串联电阻 | DLN line 0 | GPIO1 |
| GPIO smoke B | channel 7 | GPIO10 | 串联电阻 | DLN line 1 | GPIO2 |
| Ground | GND | GND | 直连 | GND | GND |

这套固定接线覆盖：

- I2C slave 测试，默认地址 `0x52`。
- SPI slave 测试，用于验证 S3 作为 Linux SPI master 的传输。
- S3 DLN line 0 和 line 1 的 GPIO 读写及 IRQ smoke。

不要再把 C3 channel 0、1、3、4、5、6 并接到 S3 DLN GPIO line 上。固定
harness 里这些 C3 引脚已经分别属于 I2C/SPI，继续并接会让测试对象混乱，也会
提高误驱动风险。

## 构建和烧录

使用 ESP-IDF v5.5.4。下面的 `IDF_PATH` 和串口名需要按你的机器修改。

```bash
export IDF_PATH=/path/to/esp-idf
. "$IDF_PATH/export.sh"
idf.py -C c3_test set-target esp32c3
idf.py -C c3_test build
idf.py -C c3_test -p /dev/ttyACM2 flash monitor
```

注意不要把 C3 固件刷到 S3 的串口。当前开发环境里 S3 通常是 `/dev/ttyACM1`，
C3 通常是 `/dev/ttyACM2`，但其它机器上可能不同。

固件运行后会打印：

```text
READY c3-probe-v4 channels=8
```

## 固定 Harness 回归

前提：

- S3 已刷入 ESP USB DLN2 固件。
- S3 native USB 已接入 Linux 主机。
- host support 已安装，`esp-usb-dln2-check` 全部为 `OK`。
- C3 已刷入本目录固件，并按固定 no-rewire harness v1 接线。

默认回归：

```bash
./scripts/c3_fixed_harness_regression.sh --c3-port /dev/ttyACM2
```

默认回归包含：

- GPIO line 0/1 的读写和 IRQ smoke。
- I2C basic transfer matrix。
- SPI mode 0 full-duplex 基础传输。
- ADC IIO smoke 由根目录的用户 smoke 流程覆盖，不依赖 C3。

扩展回归：

```bash
./scripts/c3_fixed_harness_regression.sh --c3-port /dev/ttyACM2 --with-i2c-stress --with-spi-matrix
```

或者使用更高层入口：

```bash
./scripts/full_regression.sh --c3-port /dev/ttyACM2 --extended
```

`full_regression.sh` 会把日志写入 `test_logs/`。

## 单项测试

固定 harness 的 GPIO smoke/IRQ：

```bash
./scripts/c3_closed_loop_test.py --c3-port /dev/ttyACM2 --lines "0 1" --channels "2 7"
```

I2C：

```bash
./scripts/i2c_c3_slave_test.py --c3 /dev/ttyACM2 --addr 0x52 --lengths "1 16 64"
./scripts/i2c_c3_stress_test.py --c3 /dev/ttyACM2 --addr 0x52 --bad-addr 0x53 --cycles 50
```

SPI：

```bash
./scripts/spi_c3_slave_test.py --c3 /dev/ttyACM2 --spidev /dev/spidev0.0 --mode 0 --speed 100000 --length 16 --direction full
```

SPI 脚本还支持：

- `--direction full|read|write`
- `--mode 0|1|2|3`
- `--length 1..256`

## Legacy GPIO-Only 接线

只有在需要更广的 GPIO-only 覆盖、并且固定 I2C/SPI harness 没有连接时，才使用
这套旧接线。所有信号线都要串联电阻。

| C3 channel | C3 GPIO | 串联电阻 | S3 Linux line | S3 GPIO |
| ---------- | ------- | -------- | ------------- | ------- |
| 0 | GPIO0 | 1 kOhm-4.7 kOhm | 0 | GPIO1 |
| 1 | GPIO1 | 1 kOhm-4.7 kOhm | 1 | GPIO2 |
| 2 | GPIO3 | 1 kOhm-4.7 kOhm | 2 | GPIO4 |
| 3 | GPIO4 | 1 kOhm-4.7 kOhm | 3 | GPIO5 |
| 4 | GPIO5 | 1 kOhm-4.7 kOhm | 4 | GPIO6 |
| 5 | GPIO6 | 1 kOhm-4.7 kOhm | 5 | GPIO7 |
| 6 | GPIO7 | 1 kOhm-4.7 kOhm | 6 | GPIO8 |
| 7 | GPIO10 | 1 kOhm-4.7 kOhm | 7 | GPIO9 |

Legacy GPIO-only 八路线测试：

```bash
./scripts/c3_closed_loop_test.py --c3-port /dev/ttyACM2
```

不要把 legacy GPIO-only 接线和固定 no-rewire harness 混接。

## 串口命令

C3 固件在 ESP-IDF console 上接受换行结尾命令，默认使用 USB-Serial/JTAG，
115200 baud。host 脚本会自动发送这些命令，通常不需要手工输入。

- `PING`
- `MAP`
- `STATUS`
- `READ`
- `RESET`
- `IN <mask>`
- `DRIVE <mask> <value>`
- `PULSE <mask> <count> <high_ms> <low_ms>`
- `I2C START [addr]|STOP|STATUS|RX <len>|LOAD <len>`
- `SPI START [mode]|STOP|STATUS|XFER <len> [timeout_ms]`

mask 使用 C3 channel bit。比如 channel 0 是 bit `0x01`，channel 7 是
bit `0x80`。

`RESET` 会把所有 probe channel 恢复为输入并启用 pulldown。`PULSE` 会在输出
指定脉冲后释放对应 channel 回输入状态。

## 安全要点

- 只使用 3.3 V 信号。
- C3 和 S3 必须共地。
- 不要连接两块板的 5 V 轨。
- Push-pull 信号建议串联电阻。
- I2C 必须使用 3.3 V 上拉，不能上拉到 5 V。
- 不要在固定 harness 上额外并接其它 GPIO 测试线。
