# Windows 支持

Windows 10/11 使用系统自带的 WinUSB。新固件通过 BOS / Microsoft OS 2.0
描述符提供 `WINUSB` compatible ID 和应用发现所需的 interface GUID，首次插入
即可自动绑定驱动，不需要 Zadig、Diolan 安装包或自制驱动包。

主机工具是 `scripts/dln2_windows.py`，只依赖 Python 3.10+ 标准库，直接调用
Windows SetupAPI 和 WinUSB。普通使用不需要管理员权限，也不需要 `pip install`。

## 首次更新固件

旧固件没有 Windows 自动识别描述符，必须先更新一次。连接烧录/日志 USB 和
native USB；在已经配置好的 ESP-IDF 5.5.4 PowerShell 中构建、烧录：

```powershell
idf.py build
idf.py -p COM6 flash
```

`COM6` 是示例，请确认实际 ESP32-S3 烧录串口。更新后重新插拔 native USB。
设备管理器应显示正常启动的 `DLN-2 USB GPIO`，驱动服务为 `WinUSB`，不再有
Code 28。USB 设备版本从 `0x0100` 升为 `0x0101`，用于区分旧固件的识别缓存。

若之前手动给设备装过其它驱动，自动绑定行为需要单独检查；先更新固件并重新插拔，
不要把 CH343 烧录串口的驱动改成 WinUSB。

## 使用

在项目目录中运行：

```powershell
py scripts\dln2_windows.py devices
py scripts\dln2_windows.py check
```

`check` 只读取硬件 ID、序列号、GPIO/ADC 数量和 SPI 能力，不驱动外部引脚。
多设备时使用 `py scripts\dln2_windows.py --index 1 check`；索引对应 `devices`
返回的路径顺序。

GPIO 参数是 DLN-2 line offset，不是 ESP32-S3 GPIO 编号：

| Line offset | ESP32-S3 GPIO |
| --- | --- |
| 0–9 | 1, 2, 4, 5, 6, 7, 8, 9, 18, 21 |

下面的写操作仅在确认接线安全后使用，所有信号都只能是 3.3 V。

```powershell
py scripts\dln2_windows.py gpio-read 0
py scripts\dln2_windows.py gpio-write 0 1
py scripts\dln2_windows.py gpio-release 0
```

`gpio-read` 临时设置输入，读取后释放。`gpio-write` 预先设置输出锁存值，然后
切为输出；命令结束后保持输出，直到 `gpio-release` 或设备复位。读取同一 line
也会使其变为输入。不要连接两个推挽输出。

I2C：port 0，SDA GPIO38 / SCL GPIO47，固定 100 kHz，7-bit 地址，每次最多
256 字节。写空字符串表示不发送数据的地址探测：

```powershell
py scripts\dln2_windows.py i2c-write 0x52 ""
py scripts\dln2_windows.py i2c-write 0x52 "00 12 34"
py scripts\dln2_windows.py i2c-read 0x52 4
```

I2C 写和读是独立事务，不提供跨命令 repeated START。实际数据访问需要连接从设备。

SPI：port 0 / CS0，SCLK GPIO10、MOSI GPIO11、MISO GPIO12、CS GPIO13，
8-bit，全双工，每次 1–256 字节，10 kHz–10 MHz，mode 0–3：

```powershell
py scripts\dln2_windows.py spi "9f 00 00 00" --speed 100000 --mode 0
```

ADC：channel 0–3 对应 GPIO14–17，返回 0–1023 的 raw 值，不是校准后的电压：

```powershell
py scripts\dln2_windows.py adc 0
```

I2C、SPI、ADC 单次操作结束后会禁用本次使用的端口并释放引脚。每块适配器同一
时间只由一个客户端控制。

## 在 Python 中使用

```python
import sys
sys.path.insert(0, "scripts")
from dln2_windows import DLN2, WinUSBTransport

with DLN2(WinUSBTransport()) as device:
    print(device.info())
    print(device.adc_read(0))
```

`request(handle, command, payload)` 可访问固件已经实现的其它 DLN-2 命令。
同步请求会单独排队 GPIO/ADC 异步事件，可通过 `next_event(timeout=1.0)` 读取
`(command, echo, payload)`；事件队列最多 256 条，溢出会明确报错。CLI 当前提供
单次操作，不提供 Linux IIO 风格的采样缓冲区或 GPIO IRQ 监控界面。

## Linux 兼容性

保留 VID/PID `a257:2013`、interface 0、Bulk OUT `0x01` / IN `0x81` 以及
原有 DLN-2 命令和事件。Linux 继续使用 `dln2` 及其 GPIO/I2C/SPI/ADC 子驱动，
沿用 [HOST_SUPPORT.md](HOST_SUPPORT.md) 的一次性配置。

Windows 工具使用同一份固件。原有 Linux 脚本和 OLED 仪表盘仍使用 Linux 设备
接口，不会因为安装了 WinUSB 就变成 Windows 应用。原厂 DLN-2 应用兼容性
不属于这里的验证范围。

## 开发检查

无硬件的协议、输入检查及资源释放测试：

```powershell
py -B -m unittest discover -s tests -v
```

在 ESP-IDF Python 环境中检查生成固件的实际描述符：

```powershell
python tests\check_usb_descriptors.py build\esp_usb_dln2.elf
```

仅对没有任何外部接线的裸板，以下测试会驱动全部 GPIO 输出并在结束后释放：

```powershell
py -B tests\windows_device_smoke.py --bare-board
```

该测试检查 GPIO 锁存值和方向、I2C 无从设备时的 NACK 恢复、SPI 各 mode 与
USB 包边界、ADC raw 范围，以及 ADC 异步事件与命令响应共存。它不测 GPIO
电压、不验证 I2C/SPI 从设备数据，也不校准 ADC。

实机验证需另外覆盖 Windows 驱动自动绑定、USB 通信，以及 Linux 的重新枚举和
接口回归；主机测试和描述符检查不能代替外设电气测试。

## 本次验证记录（2026-10-19）

设备：ESP32-S3，MAC `d8:3b:da:4e:28:a8`，8 MB Flash；先做裸板测试，
随后接入 AHT20 + BMP280 验证 I2C 从设备。
ESP-IDF 5.5.4 构建通过，应用镜像 308448 字节；只更新 `0x10000` 的应用分区。

| 检查 | 结果 |
| --- | --- |
| Windows 自动绑定 | 从 Code 28 自动转为 `winusb.inf` / `WINUSB`，无手动驱动安装 |
| 普通权限运行 | 系统 Python 3.14.7 可发现设备并读取身份及接口能力 |
| 设备复位后重新枚举 | WinUSB 正常启动，`check` 再次通过 |
| 主机协议测试 | Windows、Ubuntu WSL 各 24 项通过 |
| 编译产物描述符检查 | Linux interface 0 与端点不变；单接口 WinUSB 描述符及 GUID 校验通过 |
| GPIO | 10 路输入读取、输出锁存 0/1、方向读回及释放通过 |
| I2C | 23 次无从设备 NACK 与禁用/重启恢复通过，含读和 256 字节写请求 |
| I2C 真实传感器 | AHT20 `0x38`、BMP280 `0x77`（ID `0x58`）连续 5 组读取通过；AHT20 CRC 全部通过，BMP280 使用出厂校准参数换算 |
| SPI | 26 次传输通过，覆盖 mode 0–3、1/52/53/64/128/256 字节及频率边界 |
| ADC | 四通道 raw 范围检查通过，20 次异步事件与同步读命令共存通过 |

传感器接线：VCC → 3V3、GND → GND、SDA → GPIO38、SCL → GPIO47。
本次 AHT20 温度 25.997–26.042 °C、湿度 66.280–66.382 %RH；BMP280 温度
25.733–25.748 °C、当地气压 1006.938–1007.012 hPa。已校验 CRC 算法及温压换算
测试向量，并在采样后恢复 BMP280 原控制寄存器；未用参考仪器校准绝对精度。

未做 Linux 实机 USB 绑定回归、SPI 从设备数据验证、GPIO 电压测量或 ADC
精度校准。WSL 中的协议测试不代表 Linux 设备驱动实测。

本机原始完整 Flash 备份：`build/pre-windows-d83bda4e28a8.bin`（8 MB）。
裸板报告：`build/windows-device-smoke.json`；本次固件：
`build/windows/esp_usb_dln2.bin`。这些文件受 `.gitignore` 排除，不随源码分发。
传感器读数及原始字节保存在 `build/windows-sensors.json`，本机复测脚本为
`build/read_connected_sensors.py`，同样不随源码分发。

参考：[Microsoft WinUSB 自动安装](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-installation)、
[TinyUSB Microsoft OS 2.0 示例](https://github.com/hathach/tinyusb/tree/master/examples/device/webusb_serial)。
