# Technical Notes

This file records protocol details, implementation decisions, and pitfalls
found while building the ESP32-S3 DLN-2 compatible USB GPIO device.

## Hardware and Tooling

- ESP-IDF path: `/home/jzen/.espressif/v5.5.4/esp-idf/`
- Flash/log UART requested by user: `/dev/ttyACM1`
- USB device under development uses ESP32-S3 native USB D-/D+ pins
  GPIO19/GPIO20. These pins must not be exposed as GPIO lines.

## Initial Scope

- USB device: single vendor-specific interface with one bulk OUT endpoint and
  one bulk IN endpoint.
- Linux compatibility target: mainline `drivers/mfd/dln2.c`,
  `drivers/gpio/gpio-dln2.c`, and `include/linux/mfd/dln2.h`.
- First firmware scope: DLN-2 control version/serial commands plus the
  GPIO-only command subset needed for `gpiodetect`, `gpioinfo`, `gpioset`,
  and `gpioget`.

## Protocol Details Confirmed

- Linux `drivers/mfd/dln2.c` uses:
  - `struct dln2_header`: `u16 size`, `u16 id`, `u16 echo`, `u16 handle`
  - `struct dln2_response`: header plus `u16 result`
  - `CMD_GET_DEVICE_VER = DLN2_CMD(0x30, 0x00)`
  - `CMD_GET_DEVICE_SN = DLN2_CMD(0x31, 0x00)`
  - control handle `1`, GPIO handle `2`, I2C handle `3`, SPI handle `4`,
    ADC handle `5`
- `GET_DEVICE_VER` must return a 32-bit little-endian value `0x00000200`.
  Returning only a 16-bit value causes Linux hardware init to fail.
- `gpio-dln2` reads GPIO pin count as a 16-bit little-endian value.
- GPIO pin/value responses are `u16 pin` plus optional `u8 value`; set
  commands can respond with only the `u16 pin` because Linux transmit-only
  helpers ignore response payload.
- When switching a line to output, write the stored output value before enabling
  output mode and again afterward. This keeps the ESP32 output latch aligned
  with the DLN-2 shadow value and reduces input-to-output transition glitches.
- Linux `gpio-dln2` registers a callback for `DLN2_GPIO_CONDITION_MET_EV`.
  Event packets use `handle = 0` and do not include a response `result` field;
  the payload consumed by `gpio-dln2` is `u16 count`, `u8 type`, `u16 pin`,
  and `u8 value`.
- Linux maps rising/falling edge IRQ types to device-side `CHANGE` events when
  calling `SET_EVENT_CFG`; it then filters rising/falling in the host callback
  by checking the event value.
- For the multifunction phase, Linux driver source remains the protocol
  authority. Implement only the commands actually used by mainline
  `i2c-dln2`, `spi-dln2`, and `dln2-adc` first; unsupported DLN-2 commands
  should continue to return explicit error responses.
- `pico-usb-io-board` is useful as a device-side reference because it follows
  this same minimal-command approach and has a small `dln2-pin.c` ownership
  layer. The ownership model matters for ESP32-S3 because Linux creates the
  GPIO, I2C, SPI, and ADC children unconditionally after MFD probing succeeds.
- The Linux subdevice handles are not the same as command module IDs:
  GPIO handle/module `2/0x01`, I2C `3/0x03`, SPI `4/0x02`, ADC `5/0x06`.
  DLN-2 command IDs are encoded as `cmd | (module << 8)`.
- Minimal I2C Linux path:
  `ENABLE`, `WRITE`, `READ`, and `DISABLE`; command IDs are
  `DLN2_CMD(0x01, 0x03)`, `DLN2_CMD(0x06, 0x03)`,
  `DLN2_CMD(0x07, 0x03)`, and `DLN2_CMD(0x02, 0x03)`.
- Minimal SPI Linux path includes probe commands `DISABLE`, `GET_SS_COUNT`,
  `GET_MIN_FREQUENCY`, `GET_MAX_FREQUENCY`, `GET_SUPPORTED_FRAME_SIZES`,
  `SS_MULTI_ENABLE`, and `ENABLE`, plus runtime `SET_SS`, `SET_FREQUENCY`,
  `SET_MODE`, `SET_FRAME_SIZE`, `READ_WRITE`, `READ`, and `WRITE`.
- Minimal ADC Linux raw-read path includes `SET_RESOLUTION`,
  `GET_CHANNEL_COUNT`, `CHANNEL_ENABLE`, `ENABLE`, `CHANNEL_GET_VAL`,
  `DISABLE`, and `CHANNEL_DISABLE`. The Linux driver sends value `10` in the
  second byte of `SET_RESOLUTION`; despite the struct field name, this is the
  requested ADC resolution in bits.
- The firmware now has a central ESP32-S3 physical pin ownership table. GPIO
  enable claims the line's physical GPIO as module `0x01`; GPIO disable releases
  it. Future I2C/SPI/ADC enable paths must claim their physical pins before
  configuring ESP-IDF peripherals.
- Ownership conflicts should return DLN-2 result codes rather than silently
  reconfiguring pins:
  - `DLN2_RES_PIN_IN_USE` for a physical pin claimed by another module.
  - `DLN2_RES_PIN_NOT_CONNECTED_TO_MODULE` for unavailable pins or release by
    the wrong module.
  - `DLN2_RES_INVALID_PIN_NUMBER`, `DLN2_RES_INVALID_PORT_NUMBER`, and
    `DLN2_RES_INVALID_CHANNEL_NUMBER` for bad addressing.
- Because Linux currently enables all exported GPIO lines during `gpio-dln2`
  probe, any physical pin needed by a real I2C/SPI/ADC peripheral must be
  removed from the exported GPIO map before that peripheral can probe
  successfully.
- The first I2C port uses ESP32-S3 GPIO38 for SDA and GPIO47 for SCL. The WeAct
  ESP32S3-A schematic exposes both pins on the headers, and neither pin is in
  the current 18-line GPIO export.
- `DLN2_I2C_ENABLE` is enough for the Linux `i2c-dln2` probe to register an
  adapter. With GPIO38/GPIO47 available, Linux now creates `/dev/i2c-4` with
  name `dln2-i2c-3-2.1.2:1.0-0` on the current host.
- The I2C firmware path implements only Linux-used commands:
  `DLN2_CMD(0x01, 0x03)`, `DLN2_CMD(0x02, 0x03)`,
  `DLN2_CMD(0x06, 0x03)`, and `DLN2_CMD(0x07, 0x03)`.
- Linux `i2c-dln2` currently sends `mem_addr_len = 0`, so the firmware rejects
  non-zero I2C memory-address fields with `DLN2_RES_INVALID_VALUE` until a real
  host path needs that extension.
- Host runtime I2C tests need read/write permission on `/dev/i2c-*`. On this
  machine `/dev/i2c-4` is `root:i2c 660`, while user `jzen` is not in the
  `i2c` group.

## Implemented USB Shape

- Device descriptor:
  - VID `0xa257`
  - PID `0x2013`
  - product string `DLN-2 USB GPIO`
- Configuration:
  - interface 0
  - class `0xff` vendor-specific
  - endpoint `0x01` bulk OUT
  - endpoint `0x81` bulk IN
  - full-speed endpoint packet size 64 bytes
- Vendor OUT requests are framed by the DLN-2 header `size` field. The TinyUSB
  RX callback can deliver a partial request or multiple back-to-back requests,
  so the firmware accumulates bytes until one full DLN-2 frame is available and
  then dispatches it. Invalid frame sizes are dropped and the accumulator is
  reset.

## Implemented GPIO Map

| DLN-2 line | ESP32-S3 GPIO |
| ---------- | ------------- |
| 0          | GPIO1         |
| 1          | GPIO2         |
| 2          | GPIO4         |
| 3          | GPIO5         |
| 4          | GPIO6         |
| 5          | GPIO7         |
| 6          | GPIO8         |
| 7          | GPIO9         |
| 8          | GPIO10        |
| 9          | GPIO11        |
| 10         | GPIO12        |
| 11         | GPIO13        |
| 12         | GPIO14        |
| 13         | GPIO15        |
| 14         | GPIO16        |
| 15         | GPIO17        |
| 16         | GPIO18        |
| 17         | GPIO21        |

## Excluded GPIOs

- GPIO0: boot strapping pin, `KEY@BOOT`, auto-download path.
- GPIO3: strapping pin for JTAG signal source.
- GPIO19/GPIO20: native USB D-/D+ used by the DLN-2 USB device.
- GPIO35/GPIO36/GPIO37: used internally for OSPI PSRAM on ESP32-S3R8 modules.
- GPIO39/GPIO40/GPIO41/GPIO42: JTAG pin group; reserved until explicitly
  needed and tested.
- GPIO43/GPIO44: UART0 TX/RX, used for flashing and logs through CH343.
- GPIO45: strapping pin and `KEY@USER` path on the WeAct board.
- GPIO46: strapping pin.
- GPIO48: connected to the onboard WS2812 LED path.
- GPIO38/GPIO47: kept reserved in the stable map for now; both are exposed on
  the board, but they live in the high-numbered alternate-function group and
  need separate hardware validation before export.

## ESP32-C3 Closed-Loop Probe

- `c3_test` is an ESP32-C3 firmware project used as an external GPIO probe for
  the ESP32-S3 DLN-2 GPIO device.
- Default C3 probe channels:
  - channel 0: C3 GPIO0
  - channel 1: C3 GPIO1
  - channel 2: C3 GPIO3
  - channel 3: C3 GPIO4
  - channel 4: C3 GPIO5
  - channel 5: C3 GPIO6
  - channel 6: C3 GPIO7
  - channel 7: C3 GPIO10
- These C3 pins avoid common ESP32-C3 strapping pins GPIO2/GPIO8/GPIO9, native
  USB pins GPIO18/GPIO19, UART0 pins GPIO20/GPIO21, and flash pins.
- Each C3-to-S3 signal must use a series resistor, preferably 1 kOhm to
  4.7 kOhm. This limits current if a test bug ever configures both sides as
  opposing push-pull outputs.
- The first C3 probe mapping covers S3 DLN lines 0-7, which correspond to S3
  GPIO1, GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO8, and GPIO9.
- The C3 firmware defaults all probe channels to input with pulldown enabled.
  The host script only asks C3 to drive lines during the C3-output and pulse
  portions of the test, and resets C3 channels back to input afterward.
- The C3 host script waits after opening the serial port and retries `PING`
  synchronization. This handles C3 boards whose USB-UART auto-reset circuit
  toggles reset when the serial device is opened.
- For C3 USB-Serial/JTAG command input, setting
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` is not sufficient by itself. The probe
  firmware explicitly installs `usb_serial_jtag_driver` and calls
  `usb_serial_jtag_vfs_use_driver()` before using `fgets(stdin)`. Without this,
  boot logs and `printf()` output can appear on `/dev/ttyACM2`, while
  host-to-device writes time out in pyserial and `idf.py monitor`.
- Keep the C3 serial write timeout separate from the read polling timeout. A
  short read timeout is useful for command response polling, but USB CDC writes
  around reset/driver startup need a longer timeout.
- Open the C3 serial port with pyserial `exclusive=True`. Parallel probes
  against the same USB-Serial/JTAG TTY corrupt command/response streams and can
  produce misleading partial lines.
- The first closed-loop pass should use S3 DLN lines 0-7. The same C3 probe can
  cover S3 lines 8-17 later by moving the eight resistor-protected wires and
  passing the selected line offsets with `--lines`.
- If C3 GPIO10 / probe channel 7 is not wired, run the test with
  `--lines "0 1 2 3 4 5 6"` so the script only drives and samples channels
  0-6. This seven-channel setup was verified on `/dev/ttyACM2` against S3 DLN
  lines 0-6.

## Verification Notes

- The host automatically bound the mainline `dln2` driver with VID/PID
  `a257:2013`; no `new_id` binding was required.
- Verified result:
  - `/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00`
  - `chip name=gpiochip2 label=dln2 lines=8`
  - line 0 input read succeeded
- After installing libgpiod v2.2.4, verified:
  - `gpiodetect`
  - `gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00`
  - `gpioget -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 --numeric 0`
  - `gpioget -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 --numeric 0 1 2 3 4 5 6 7`
  - `gpioset -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -t 100ms,0 0=1`
  - `gpiomon -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -e both --debounce-period 10ms --idle-timeout 1s 0`
- After expanding the safe GPIO map to 18 lines, verified:
  - `gpiodetect` reports `gpiochip2 [dln2] (18 lines)`.
  - `gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17`
  - `gpioget -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 --numeric 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17`
  - `gpioset -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -t 100ms,0 17=1`
  - `gpiomon -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -e both --debounce-period 10ms --idle-timeout 1s 17`
- The kernel also autoloads I2C, SPI, and ADC DLN-2 subdrivers. The firmware
  currently returns `DLN2_RES_NOT_IMPLEMENTED` for those valid but unsupported
  handles.

## Pitfalls

- Do not use USB CDC logging in the first version; UART logs are safer while
  debugging native USB enumeration.
- Always respond to unknown DLN-2 commands with an error packet. Linux timeouts
  make protocol debugging much slower than explicit errors.
- Do not assume one USB vendor RX callback equals one DLN-2 request. USB bulk
  transfers preserve byte order, not application message boundaries.
- Keep DLN-2 line numbers separate from ESP32-S3 GPIO numbers through a fixed
  mapping table.
- ESP-IDF monitor requires a TTY. Running it through a normal pipe fails with
  "Monitor requires standard input to be attached to TTY".
- The board reports 8 MB flash, while the current image header is configured
  for 2 MB. ESP-IDF warns and uses the image header size. This does not block
  the current MVP but should be cleaned up later if larger partitions are
  needed.
- libgpiod v2 syntax is different from older examples: use `-c <chip>` and
  pass offsets as separate arguments, for example `gpioget -c gpiochip2 0`.
- `gpioset -p <period>` does not mean "exit after this period" in libgpiod
  v2. It keeps the line requested after the minimum hold period. Use
  `-t 100ms,0` for a short finite output pulse.
- After `gpioset` exits, `gpioinfo` may briefly report the released line as
  output with no consumer. The Linux `gpio-dln2` driver caches direction and
  does not clear its `output_enabled` bitmap in the free path. A subsequent
  input request, such as `gpioget`, refreshes it.
- `scripts/gpio_pin_walk.sh` uses the Python `gpiod` binding instead of
  `gpioset -t` so it can print one log line after every set-high and set-low
  operation. On Ctrl-C or SIGTERM it attempts to set all selected lines low
  before releasing the request.
- Linux GPIO line offsets are DLN-2 line numbers, not ESP32-S3 GPIO numbers.
  In the current firmware, line 0 maps to ESP32 GPIO1 and line 17 maps to
  ESP32 GPIO21.
- On the WeAct ESP32S3-A schematic, GPIO0 is tied to `KEY@BOOT` and the
  auto-download circuit path. GPIO0 and GPIO3 are also ESP32-S3 strapping pins,
  so keeping them out of the first stable GPIO export avoids boot-mode and
  reset-time surprises.
- Avoid jumper loopback tests as a routine validation path. They can create
  push-pull contention if the wrong two lines are configured as outputs. Prefer
  non-destructive host-side validation such as `gpioget`, `gpioset` on a single
  line, `gpioinfo`, and `gpiomon` event enable/idle-timeout checks.
- For stronger validation, use the ESP32-C3 probe with per-line series
  resistors instead of direct jumper loopbacks. This provides a controllable
  external input/output source while keeping accidental contention current
  limited.
- GPIO events now use ESP GPIO interrupts plus a FreeRTOS queue. The ISR reads
  the changed level and queues `{pin, value}` only; debounce filtering, DLN-2
  event packet construction, and USB writes run later in task context.
- `SET_EVENT_CFG` updates the ESP GPIO interrupt registration. Event type
  `CHANGE` maps to `GPIO_INTR_ANYEDGE`; level-high/level-low DLN event types
  are handled as rising/falling edge entry events to avoid continuous level IRQ
  storms.
- USB event writes must stay out of ISR context. The event task waits for queued
  GPIO events and drains any immediately available events while the vendor IN
  endpoint has room.
