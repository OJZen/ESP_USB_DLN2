下面是加入参考资料后的精简技术方案版。

# ESP32-S3 模拟 DLN-2 USB GPIO 控制器技术方案

## 1. 目标

使用 **ESP32-S3 + ESP-IDF + TinyUSB** 模拟一个 **DLN-2 兼容 USB GPIO 控制器**。

Linux 端复用主线内核已有驱动：

```text
dln2.ko
gpio-dln2.ko
```

最终目标是在 Linux 上出现：

```text
/dev/gpiochipX
```

并可以直接使用：

```bash
gpiodetect
gpioinfo
gpioset
gpioget
```

---

## 2. 参考资料

### 2.1 设备端模拟参考

优先参考：

```text
ipmgroup/pico-usb-io-board
notro/pico-usb-io-board
```

这两个项目都是 Raspberry Pi Pico 方向的 DLN-2 设备端实现。`notro/pico-usb-io-board` 的 README 明确说明它把 Pico 变成 USB I/O Board，并实现了 Linux `dln2` 驱动使用的 USB 协议，同时支持 2 个 CDC UART；但原仓库已归档，只读。`ipmgroup/pico-usb-io-board` 是后续 fork，文件结构更值得先看。([GitHub][1])

重点文件：

```text
dln2.c              # DLN-2 协议收发
dln2.h              # 协议结构体 / 命令定义
dln2-gpio.c         # GPIO 命令实现
dln2-devices.h      # handle / module 定义
dln2-pin.c          # pin 管理
usb_descriptors.c   # USB 描述符
tusb_config.h       # TinyUSB 配置
driver.c            # USB endpoint / 设备主循环相关
```

`ipmgroup` fork 里能看到 `dln2-adc.c`、`dln2-gpio.c`、`dln2-i2c.c`、`dln2-spi.c`、`dln2.c`、`dln2.h`、`usb_descriptors.c` 等文件，适合直接按模块拆解移植。([GitHub][2])

### 2.2 协议权威参考

参考 Diolan 官方文档：

```text
DLN-Series Interface Adapters Programmer's Reference Manual
```

里面包含 DLN 系列的 GPIO、I2C、SPI、ADC 等 API/命令语义，例如 GPIO pin count 等接口说明。([diolan.com][3])

### 2.3 Linux 实际兼容目标

最终不是兼容“理论 DLN-2”，而是兼容当前 Linux 内核里的这些文件：

```text
drivers/mfd/dln2.c
drivers/gpio/gpio-dln2.c
include/linux/mfd/dln2.h
```

其中：

```text
drivers/mfd/dln2.c        # USB 主驱动，负责枚举、bulk 收发、MFD 子设备创建
drivers/gpio/gpio-dln2.c  # GPIO 子驱动，负责注册 gpiochip
include/linux/mfd/dln2.h  # 公共结构、命令接口
```

Linux 主线仓库中这些文件可以作为协议行为的最终判断依据。([GitHub][4])

### 2.4 ESP32-S3 USB/TinyUSB 参考

ESP-IDF 的 ESP32-S3 USB Device Stack 基于 TinyUSB，支持 vendor-specific class、复合设备、自定义 descriptor 等功能。官方文档还说明 ESP32-S3 的 USB D+ / D- 分别连接到 GPIO20 / GPIO19。([Espressif Systems][5])

`esp_tinyusb` 组件支持自定义 USB descriptor、CDC-ACM、MSC 等，并通过 `tinyusb_driver_install()` 安装 USB Device Stack。([ESP 组件注册中心][6])

---

## 3. 总体架构

```text
Linux 用户态
  |
  | libgpiod / gpiodetect / gpioset / gpioget
  v
Linux GPIO chardev
  |
  v
gpio-dln2.ko
  |
  v
dln2.ko
  |
  | USB Bulk IN / Bulk OUT
  v
ESP32-S3 TinyUSB Vendor Device
  |
  v
DLN-2 协议解析层
  |
  v
ESP-IDF GPIO Driver
  |
  v
ESP32-S3 GPIO
```

ESP32-S3 端不是写 Linux 驱动，而是写 **DLN-2 设备端响应逻辑**：

```text
Linux dln2/gpio-dln2：发命令，等响应
ESP32-S3 固件：收命令，执行 GPIO 操作，回响应
```

---

## 4. USB 设备要求

### 4.1 实验阶段 VID/PID

为了让 Linux 自动绑定 `dln2` 驱动，实验阶段可以模拟：

```text
VID = 0xA257
PID = 0x2013
```

这是 Linux `dln2` 驱动默认识别的 DLN-2 USB ID。正式产品不要使用这个 VID/PID。

如果使用参考项目里的 Openmoko ID：

```text
VID = 0x1D50
PID = 0x6170
```

需要注意它只能用于 FOSS license 场景；参考项目 README 也明确说明了这一点。([GitHub][2])

Linux 侧临时绑定自定义 VID/PID 可以用：

```bash
sudo modprobe dln2
echo 1d50 6170 | sudo tee /sys/bus/usb/drivers/dln2/new_id
```

### 4.2 USB interface / endpoint

ESP32-S3 TinyUSB 端至少提供：

```text
Interface 0
  Bulk OUT endpoint
  Bulk IN endpoint
```

第一版不要做太复杂的复合设备，先做单 interface vendor-specific device。

---

## 5. DLN-2 协议核心

Linux `dln2` 驱动使用的通用包头：

```c
struct dln2_header {
    __le16 size;
    __le16 id;
    __le16 echo;
    __le16 handle;
};

struct dln2_response {
    struct dln2_header hdr;
    __le16 result;
};
```

ESP32-S3 固件处理流程：

```text
USB Bulk OUT 收包
        ↓
解析 dln2_header
        ↓
根据 handle + id 分发
        ↓
执行 GPIO 操作
        ↓
构造 dln2_response
        ↓
USB Bulk IN 回包
```

关键注意：

```text
size 使用 little-endian
id 是命令号
echo 必须原样返回
handle 区分 CTRL / GPIO / I2C / SPI / ADC
result 表示执行结果
未知命令也要返回错误，不要 timeout
```

---

## 6. 最小命令集

第一版建议只做 GPIO-only MVP。

### 6.1 控制类命令

至少实现：

```text
CMD_GET_DEVICE_VER
CMD_GET_DEVICE_SN
```

`CMD_GET_DEVICE_VER` 建议返回：

```text
0x0200
```

序列号可以先返回固定值：

```text
0x12345678
```

### 6.2 GPIO 命令

建议第一版实现：

```text
DLN2_GPIO_GET_PIN_COUNT
DLN2_GPIO_PIN_ENABLE
DLN2_GPIO_PIN_DISABLE
DLN2_GPIO_PIN_SET_DIRECTION
DLN2_GPIO_PIN_GET_DIRECTION
DLN2_GPIO_PIN_GET_VAL
DLN2_GPIO_PIN_SET_OUT_VAL
DLN2_GPIO_PIN_GET_OUT_VAL
DLN2_GPIO_PORT_GET_VAL
```

先不实现：

```text
GPIO IRQ
debounce
event callback
I2C
SPI
ADC
PWM
CDC UART 复合功能
```

GPIO 中断、I2C、SPI、ADC 可以后续参考 `pico-usb-io-board` 中对应模块逐步移植。

---

## 7. ESP32-S3 GPIO 映射

不要把 DLN-2 line number 直接等同于 ESP32-S3 GPIO number。建议用固定映射表：

```c
static const gpio_num_t dln2_gpio_map[] = {
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7,
    GPIO_NUM_8,
    GPIO_NUM_9,
};
```

映射关系：

```text
DLN2 line 0 -> ESP32 GPIO1
DLN2 line 1 -> ESP32 GPIO2
DLN2 line 2 -> ESP32 GPIO4
...
```

需要排除：

```text
GPIO19 / GPIO20，USB D- / D+
Flash / PSRAM 相关引脚
strapping 启动相关引脚
开发板已占用引脚
不适合外部直接使用的引脚
```

`DLN2_GPIO_GET_PIN_COUNT` 返回映射表长度即可。第一版建议先暴露 8 或 16 路，跑通后再扩展。

---

## 8. Pico 到 ESP-IDF 的移植对照

| Pico SDK / 参考项目              | ESP32-S3 / ESP-IDF                         |
| ---------------------------- | ------------------------------------------ |
| `gpio_init(pin)`             | `gpio_config()`                            |
| `gpio_set_dir(pin, out)`     | `gpio_set_direction()`                     |
| `gpio_put(pin, val)`         | `gpio_set_level()`                         |
| `gpio_get(pin)`              | `gpio_get_level()`                         |
| `gpio_get_out_level(pin)`    | 自己维护 output shadow                         |
| `pico_get_unique_board_id()` | `esp_efuse_mac_get_default()`              |
| Pico TinyUSB init            | `esp_tinyusb` / `tinyusb_driver_install()` |
| `usb_descriptors.c`          | 保留结构，改 VID/PID、endpoint、字符串描述符             |

建议内部维护 GPIO 状态：

```c
typedef struct {
    gpio_num_t esp_gpio;
    bool enabled;
    bool is_output;
    bool out_value;
} dln2_gpio_line_t;

static dln2_gpio_line_t lines[DLN2_GPIO_COUNT];
```

`out_value` 用于响应 `DLN2_GPIO_PIN_GET_OUT_VAL`。

---

## 9. 固件结构建议

```text
main/
  main.c
  usb_descriptors.c
  tusb_config.h

components/dln2/
  dln2.h
  dln2.c
  dln2_protocol.h
  dln2_gpio.h
  dln2_gpio.c
  dln2_pinmap.h
  dln2_pinmap.c
```

`dln2.c` 负责：

```text
USB 收发
DLN-2 header 解析
handle + id 命令分发
response 打包
错误返回
```

`dln2_gpio.c` 负责：

```text
GPIO pin count
GPIO enable / disable
GPIO direction
GPIO read
GPIO write
GPIO output shadow
```

---

## 10. Linux 端验证

插入设备后：

```bash
lsusb
lsusb -d a257:2013 -v
sudo dmesg -w
```

确认模块：

```bash
modinfo dln2
modinfo gpio-dln2
sudo modprobe dln2
sudo modprobe gpio-dln2
```

验证 GPIO：

```bash
gpiodetect
gpioinfo
gpioset /dev/gpiochip0 0=1
gpioset /dev/gpiochip0 0=0
gpioget /dev/gpiochip0 1
```

理想结果类似：

```text
gpiochip0 [dln2-gpio] (16 lines)
```

或者：

```text
gpiochip0 [dln2-gpio] (32 lines)
```

---

## 11. 调试重点

最容易出问题的是：

```text
VID/PID 不匹配
interface 编号不符合 Linux dln2 预期
endpoint 不是 bulk IN / bulk OUT
DLN-2 size 字段错误
echo 没有原样返回
handle 错误
response payload 长度错误
大小端错误
未知命令没有响应，导致 Linux timeout
GPIO pin count 返回异常
```

ESP32-S3 建议通过 UART 打印：

```text
RX: size, id, echo, handle
TX: id, echo, handle, result, payload_len
GPIO: line, esp_gpio, direction, value
UNKNOWN CMD: handle, id
```

不建议第一版用 USB CDC 打日志，因为 USB 本身就是被测试对象。

---

## 12. 推荐参考优先级

按这个顺序看：

```text
1. ipmgroup/pico-usb-io-board
   看设备端实现，尤其 dln2.c / dln2-gpio.c / usb_descriptors.c

2. notro/pico-usb-io-board
   原始实现，虽然已归档，但仍是核心参考

3. Linux drivers/mfd/dln2.c
   看 Linux 主驱动如何枚举、发命令、等响应

4. Linux drivers/gpio/gpio-dln2.c
   看 GPIO 子驱动实际调用哪些命令

5. include/linux/mfd/dln2.h
   看公共结构、handle、接口定义

6. Diolan Programmer's Reference Manual
   对照协议语义、返回值和边界条件

7. ESP-IDF USB Device Stack / esp_tinyusb
   负责 ESP32-S3 上 TinyUSB vendor device 落地
```

---

## 13. 结论

这条路线的关键不是写 Linux 驱动，而是把 ESP32-S3 做成一个 **DLN-2 设备端协议响应器**：

```text
ESP32-S3 TinyUSB vendor bulk device
        +
DLN-2 packet parser/responder
        +
ESP-IDF GPIO backend
```

建议第一版只做：

```text
USB 枚举
GET_DEVICE_VER
GET_DEVICE_SN
GPIO_GET_PIN_COUNT
GPIO direction
GPIO get
GPIO set
```

跑通 `/dev/gpiochipX` 和 `gpioset/gpioget` 后，再考虑 GPIO IRQ、I2C、SPI、ADC。

[1]: https://github.com/notro/pico-usb-io-board "GitHub - notro/pico-usb-io-board: Raspberry Pi Pico USB I/O Board · GitHub"
[2]: https://github.com/ipmgroup/pico-usb-io-board "GitHub - ipmgroup/pico-usb-io-board: Raspberry Pi Pico USB I/O Board · GitHub"
[3]: https://diolan.com/media/wysiwyg/downloads/dln-api-manual.pdf?utm_source=chatgpt.com "DLN-Series Interface Adapters. Programmer's Reference ..."
[4]: https://github.com/torvalds/linux/blob/master/drivers/mfd/dln2.c "linux/drivers/mfd/dln2.c at master · torvalds/linux · GitHub"
[5]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/usb_device.html "USB Device Stack - ESP32-S3 -  — ESP-IDF Programming Guide v6.0.1 documentation"
[6]: https://components.espressif.com/components/espressif/esp_tinyusb/versions/2.1.0/readme "
    espressif/esp_tinyusb • v2.1.0
•
    ESP Component Registry"
