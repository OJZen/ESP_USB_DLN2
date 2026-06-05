# DLN-2 Feature Implementation Plan

This plan keeps implementation source-driven: Linux driver code is the protocol
authority, and `notro/pico-usb-io-board` is the device-side reference for
minimal command coverage and resource ownership.

## References

- Linux `drivers/mfd/dln2.c`:
  https://github.com/torvalds/linux/blob/master/drivers/mfd/dln2.c
- Linux `drivers/gpio/gpio-dln2.c`:
  https://github.com/torvalds/linux/blob/master/drivers/gpio/gpio-dln2.c
- Linux `drivers/i2c/busses/i2c-dln2.c`:
  https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-dln2.c
- Linux `drivers/spi/spi-dln2.c`:
  https://github.com/torvalds/linux/blob/master/drivers/spi/spi-dln2.c
- Linux `drivers/iio/adc/dln2-adc.c`:
  https://github.com/torvalds/linux/blob/master/drivers/iio/adc/dln2-adc.c
- `notro/pico-usb-io-board` device firmware:
  https://github.com/notro/pico-usb-io-board

## Global Protocol Facts

- DLN-2 packet header is `u16 size`, `u16 id`, `u16 echo`, `u16 handle`.
- Response header adds `u16 result`.
- Event packets use handle `0` and do not include a response `result`.
- Linux MFD creates these subdevices unconditionally after generic control
  probing succeeds:
  - handle `2`: GPIO
  - handle `3`: I2C
  - handle `4`: SPI
  - handle `5`: ADC
- Command module IDs differ from handles:
  - GPIO module ID: `0x01`
  - SPI module ID: `0x02`
  - I2C module ID: `0x03`
  - ADC module ID: `0x06`
- Command ID encoding is `DLN2_CMD(cmd, module) = cmd | (module << 8)`.

## Implementation Order

1. Add a central pin ownership layer and convert existing GPIO enable/disable
   paths to claim and release physical ESP32-S3 pins through it.
2. Lock a v2 board pin map that reserves separate pins for I2C, SPI, and ADC
   before enabling those Linux subdrivers successfully.
3. Implement I2C probe and runtime read/write commands first. This is the
   smallest non-GPIO Linux path.
4. Implement SPI probe commands, then 8-bit runtime setup and transfer
   commands.
5. Implement ADC direct raw reads. Add buffered ADC event support only after raw
   IIO sysfs reads are stable.

## Resource Ownership

Current state:

- A central pin ownership layer exists in `components/dln2/dln2_pin.c`.
- Existing GPIO enable/disable paths claim and release their physical
  ESP32-S3 pins.

Before enabling I2C/SPI/ADC, keep extending this layer similar to
`pico-usb-io-board/dln2-pin.c`.

Requirements:

- Track each physical ESP32-S3 GPIO owner: free, GPIO, I2C, SPI, ADC, or
  unavailable.
- Reject requests that would claim a pin already owned by a different module.
- Return DLN-2-style errors for ownership conflicts, especially:
  - pin in use
  - invalid pin
  - pin not connected to module
  - invalid port/channel
- Keep peripheral pins out of the exported GPIO line map where practical. Linux
  `gpio-dln2` enables all exported GPIO lines during probe, so sharing physical
  pins between exported GPIO and I2C/SPI/ADC is a real conflict.

Open hardware decision:

- Define a stable v2 pin map that partitions ESP32-S3 pins into:
  - exported GPIO lines
  - I2C SDA/SCL
  - SPI SCLK/MOSI/MISO/CS
  - ADC-capable inputs
- ESP32-S3 ADC-capable pins include ADC1 GPIO1-GPIO10 and ADC2 GPIO11-GPIO20.
  GPIO19/GPIO20 are native USB and must remain excluded.

## GPIO / IRQ

Current state:

- GPIO basic commands are implemented.
- GPIO event config and `DLN2_GPIO_CONDITION_MET_EV` are implemented.
- Events use ESP GPIO ISR plus a FreeRTOS queue.
- C3 closed-loop testing verified S3 lines 0-6 for output, input, and per-line
  `gpiomon` events.

Remaining GPIO work:

- Add event queue overflow logging/counter.
- Add a long-running event stress test.
- Keep C3 line coverage expanding as wiring changes.

## I2C Minimal Implementation

Linux probe path:

- `DLN2_I2C_ENABLE` (`DLN2_CMD(0x01, 0x03)`)
  - TX: `u8 port`
  - RX: no payload

Runtime transfer path:

- `DLN2_I2C_WRITE` (`DLN2_CMD(0x06, 0x03)`)
  - TX: `u8 port`, `u8 addr`, `u8 mem_addr_len`, `u32 mem_addr`,
    `u16 buf_len`, `u8 buf[buf_len]`
  - RX: no payload
  - Linux expects success to mean `buf_len` bytes written.
- `DLN2_I2C_READ` (`DLN2_CMD(0x07, 0x03)`)
  - TX: `u8 port`, `u8 addr`, `u8 mem_addr_len`, `u32 mem_addr`,
    `u16 buf_len`
  - RX: `u16 buf_len`, `u8 buf[buf_len]`
  - Linux rejects replies whose returned length does not match the requested
    length.
- `DLN2_I2C_DISABLE` (`DLN2_CMD(0x02, 0x03)`)
  - TX: `u8 port`
  - RX: no payload

First target:

- One I2C port, port `0`.
- 100 kHz default bus.
- No emulated internal I2C devices initially.
- Validate Linux creates `/dev/i2c-*`.
- Validate with either C3 I2C slave firmware or a simple external I2C device.

Not needed for first Linux path:

- `SCAN_DEVICES`
- `PULLUP_*`
- `TRANSFER`
- `MAX_REPLY_COUNT`

## SPI Minimal Implementation

Linux probe path:

- `DLN2_SPI_DISABLE` (`DLN2_CMD(0x12, 0x02)`)
  - TX: `u8 port`, `u8 wait_for_completion`
  - RX: no payload
- `DLN2_SPI_GET_SS_COUNT` (`DLN2_CMD(0x44, 0x02)`)
  - TX: `u8 port`
  - RX: `u16 cs_count`
- `DLN2_SPI_GET_MIN_FREQUENCY` (`DLN2_CMD(0x45, 0x02)`)
  - TX: `u8 port`
  - RX: `u32 speed`
- `DLN2_SPI_GET_MAX_FREQUENCY` (`DLN2_CMD(0x46, 0x02)`)
  - TX: `u8 port`
  - RX: `u32 speed`
- `DLN2_SPI_GET_SUPPORTED_FRAME_SIZES` (`DLN2_CMD(0x43, 0x02)`)
  - TX: `u8 port`
  - RX: `u8 count`, `u8 frame_sizes[36]`
- `DLN2_SPI_SS_MULTI_ENABLE` (`DLN2_CMD(0x38, 0x02)`)
  - TX: `u8 port`, `u8 cs_mask`
  - RX: no payload
- `DLN2_SPI_ENABLE` (`DLN2_CMD(0x11, 0x02)`)
  - TX: `u8 port`
  - RX: no payload

Runtime setup path:

- `DLN2_SPI_SET_SS` (`DLN2_CMD(0x26, 0x02)`)
  - TX: `u8 port`, `u8 cs`
  - Linux sends inverted CS mask: selected CS bits are zero.
- `DLN2_SPI_SET_FREQUENCY` (`DLN2_CMD(0x18, 0x02)`)
  - TX: `u8 port`, `u32 speed`
  - RX: `u32 actual_speed`
- `DLN2_SPI_SET_MODE` (`DLN2_CMD(0x14, 0x02)`)
  - TX: `u8 port`, `u8 mode`
  - RX: no payload
- `DLN2_SPI_SET_FRAME_SIZE` (`DLN2_CMD(0x16, 0x02)`)
  - TX: `u8 port`, `u8 bpw`
  - RX: no payload

Runtime transfer path:

- `DLN2_SPI_READ_WRITE` (`DLN2_CMD(0x1A, 0x02)`)
  - TX: `u8 port`, `u16 size`, `u8 attr`, `u8 buf[size]`
  - RX: `u16 size`, `u8 buf[size]`
- `DLN2_SPI_READ` (`DLN2_CMD(0x1B, 0x02)`)
  - TX: `u8 port`, `u16 size`, `u8 attr`
  - RX: `u16 size`, `u8 buf[size]`
- `DLN2_SPI_WRITE` (`DLN2_CMD(0x1C, 0x02)`)
  - TX: `u8 port`, `u16 size`, `u8 attr`, `u8 buf[size]`
  - RX: no payload

First target:

- One SPI port, one CS.
- 8 bits per word first; advertise only 8-bit support initially unless Linux
  requires more.
- SPI mode 0-3 by mapping CPOL/CPHA into ESP-IDF SPI settings.
- Validate with C3 SPI slave firmware or `spidev` plus a known loopback-safe
  fixture.

## ADC Minimal Implementation

Linux probe path:

- `DLN2_ADC_SET_RESOLUTION` (`DLN2_CMD(0x08, 0x06)`)
  - TX: `u8 port`, `u8 chan`
  - The Linux struct names the second byte `chan`, but for this command Linux
    sends `10` to request fixed 10-bit resolution.
  - RX: no payload
- `DLN2_ADC_GET_CHANNEL_COUNT` (`DLN2_CMD(0x01, 0x06)`)
  - TX: `u8 port`
  - RX: `u8 count`
- Linux also registers an event callback for `DLN2_ADC_CONDITION_MET_EV`.

Direct raw read path:

- `DLN2_ADC_CHANNEL_ENABLE` (`DLN2_CMD(0x05, 0x06)`)
  - TX: `u8 port`, `u8 chan`
  - RX: no payload
- `DLN2_ADC_ENABLE` (`DLN2_CMD(0x02, 0x06)`)
  - TX: `u8 port`
  - RX: `u16 conflict_mask`
- `DLN2_ADC_CHANNEL_GET_VAL` (`DLN2_CMD(0x0A, 0x06)`)
  - TX: `u8 port`, `u8 chan`
  - RX: `u16 value`
  - Linux calls this twice after enabling a channel and uses the second value.
- `DLN2_ADC_DISABLE` (`DLN2_CMD(0x03, 0x06)`)
  - TX: `u8 port`
  - RX: zero or `u16 conflict_mask`
- `DLN2_ADC_CHANNEL_DISABLE` (`DLN2_CMD(0x06, 0x06)`)
  - TX: `u8 port`, `u8 chan`
  - RX: no payload

Buffered read path:

- `DLN2_ADC_CHANNEL_SET_CFG` (`DLN2_CMD(0x0C, 0x06)`)
  - TX: `u8 port`, `u8 chan`, `u8 type`, `u16 period`, `u16 low`,
    `u16 high`
  - RX: no payload
- `DLN2_ADC_CHANNEL_GET_ALL_VAL` (`DLN2_CMD(0x0B, 0x06)`)
  - TX: `u8 port`
  - RX: `u16 channel_mask`, `u16 values[8]`
- `DLN2_ADC_CONDITION_MET_EV` (`DLN2_CMD(0x10, 0x06)`)
  - Event payload is consumed only to trigger the IIO buffer handler; the
    Linux driver reads fresh values with `GET_ALL_VAL`.

First target:

- Direct raw reads only.
- Advertise a small channel count that matches the v2 ADC pin map.
- Return 10-bit values, scaling ESP-IDF ADC readings down if needed.
- Validate via IIO sysfs raw files before attempting triggered buffers.
