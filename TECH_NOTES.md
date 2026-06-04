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

## Verification Notes

- The host automatically bound the mainline `dln2` driver with VID/PID
  `a257:2013`; no `new_id` binding was required.
- `gpiodetect`, `gpioinfo`, `gpioset`, and `gpioget` were not installed on
  this host during testing. Equivalent GPIO chardev ioctls were used instead.
- Verified result:
  - `/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00`
  - `chip name=gpiochip2 label=dln2 lines=8`
  - line 0 input read succeeded
- The kernel also autoloads I2C, SPI, and ADC DLN-2 subdrivers. The firmware
  currently returns `DLN2_RES_NOT_IMPLEMENTED` for those valid but unsupported
  handles.

## Pitfalls

- Do not use USB CDC logging in the first version; UART logs are safer while
  debugging native USB enumeration.
- Always respond to unknown DLN-2 commands with an error packet. Linux timeouts
  make protocol debugging much slower than explicit errors.
- Keep DLN-2 line numbers separate from ESP32-S3 GPIO numbers through a fixed
  mapping table.
- ESP-IDF monitor requires a TTY. Running it through a normal pipe fails with
  "Monitor requires standard input to be attached to TTY".
- The board reports 8 MB flash, while the current image header is configured
  for 2 MB. ESP-IDF warns and uses the image header size. This does not block
  the current MVP but should be cleaned up later if larger partitions are
  needed.
