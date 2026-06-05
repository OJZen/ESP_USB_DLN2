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
- ADC buffered mode uses Linux IIO triggered-buffer plumbing. The driver
  enables selected channels through `update_scan_mode`, enables the ADC port in
  `postenable`, then sends `CHANNEL_SET_CFG` on the first active channel with
  event type `ALWAYS` and a millisecond period derived from
  `sampling_frequency`. Each `CONDITION_MET_EV` only triggers the kernel; the
  kernel then reads a synchronized frame with `GET_ALL_VAL`.
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
  the current GPIO export.
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
  machine `/dev/i2c-4` is `root:i2c 660`; adding user `jzen` to group `i2c`
  and running commands through `newgrp i2c` is enough for access.
- `i2cdetect -y 4` warns that SMBus Quick is not supported because Linux
  `i2c-dln2` does not advertise `I2C_FUNC_SMBUS_QUICK`. Use
  `i2cdetect -r -y 4` for a complete read-byte scan.
- SPI port 0 uses ESP32-S3 GPIO10 as SCLK, GPIO11 as MOSI, GPIO12 as MISO, and
  GPIO13 as CS0. These pins were removed from the exported GPIO map before
  enabling `spi-dln2`; otherwise `gpio-dln2` claims them during probe and SPI
  enable would fail with `DLN2_RES_PIN_IN_USE`.
- The SPI firmware advertises one chip select, 10 kHz to 10 MHz, and only
  8-bit frame size support. Linux `spi-dln2` accepts this and registers
  `/sys/class/spi_master/spi0`.
- ESP-IDF SPI master non-DMA mode is limited by the hardware FIFO. Use
  `SPI_DMA_CH_AUTO` on the S3 SPI bus so the firmware can support the 256-byte
  maximum transfer size used by Linux `spi-dln2`.
- A Linux SPI controller does not automatically create `/dev/spidev*`. Actual
  SPI transfer tests need a described SPI slave device or a C3 SPI slave test
  fixture.
- ADC port 0 uses ESP32-S3 GPIO14-GPIO17 as ADC2 channels 3-6. The firmware
  advertises 4 ADC channels to Linux and returns 10-bit raw values by reading
  ESP-IDF ADC oneshot at the chip's default/max bit width and shifting down.
- GPIO14-GPIO17 are removed from the exported GPIO map before enabling
  `dln2-adc`; otherwise `gpio-dln2` would claim them during GPIO probe and ADC
  channel enable would fail with `DLN2_RES_PIN_IN_USE`.
- Linux registers the ADC as an IIO device, currently
  `/sys/bus/iio/devices/iio:device0` on this host, with raw files
  `in_voltage0_raw` through `in_voltage3_raw`.
- Floating ADC inputs can read any value, including full-scale 1023. Treat
  current sysfs reads as command-path validation unless the ADC pin is connected
  to a known voltage within ESP32-S3 limits.
- The firmware implements ADC buffer events with an ESP timer and a FreeRTOS
  queue. Event payload is empty because Linux `dln2-adc` ignores event payload
  and immediately calls `GET_ALL_VAL` from its trigger handler.
- The shared USB event task now asks both ADC and GPIO event producers for
  packets. ADC/GPIO queue polling is interleaved so a fast ADC buffer stream
  does not permanently starve GPIO IRQ events.

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
| 8          | GPIO18        |
| 9          | GPIO21        |

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
- GPIO38/GPIO47: reserved for I2C port 0 SDA/SCL.
- GPIO10/GPIO11/GPIO12/GPIO13: reserved for SPI port 0
  SCLK/MOSI/MISO/CS0.
- GPIO14/GPIO15/GPIO16/GPIO17: reserved for ADC port 0 channels 0-3.

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
  cover the remaining current GPIO lines 8-9 later by moving the
  resistor-protected wires and passing the selected line offsets with
  `--lines`.
- If C3 GPIO10 / probe channel 7 is not wired, run the test with
  `--lines "0 1 2 3 4 5 6"` so the script only drives and samples channels
  0-6. This seven-channel setup was verified on `/dev/ttyACM2` against S3 DLN
  lines 0-6.

## ESP32-C3 SPI Fixture

- `c3_test` version `c3-probe-v3` is the current command-driven SPI slave
  fixture while preserving the GPIO probe protocol. Version `v2` added the
  fixture; `v3` added optional SPI mode selection.
- SPI fixture pins:
  - C3 GPIO4: SCLK input from S3 GPIO10.
  - C3 GPIO5: MOSI input from S3 GPIO11.
  - C3 GPIO6: MISO output to S3 GPIO12.
  - C3 GPIO7: CS input from S3 GPIO13.
- `SPI START [mode]` releases all GPIO probe pins to input first, then
  initializes C3 `SPI2_HOST` as a DMA SPI slave in mode 0-3 with a maximum
  scripted transfer length of 256 bytes. DMA is needed because ESP-IDF's
  non-DMA SPI slave path is limited to the hardware FIFO size, 64 bytes.
- `SPI XFER <len> [timeout_ms]` arms exactly one SPI slave transaction. The C3
  sends a fixed MISO pattern `a0 a1 ...` and records the MOSI bytes received
  from the S3. The command response includes both `rx=<hex>` and `tx=<hex>` so
  the host script can verify both directions.
- Linux `spi-dln2` only registers a SPI controller. A `/dev/spidev*` node
  appears only after some SPI child device is registered on that controller.
- `tools/spi_test_device` contains a temporary lab-only kernel module that finds the
  bound `dln2-spi` platform device, retrieves its `spi_controller`, and calls
  `spi_new_device()` with modalias `dh2228fv`, one of the SPI IDs accepted by
  the mainline `spidev` driver.
- A generic `spidev` modalias can leave `spi0.0` present but unbound on this
  host. Use the helper default `dh2228fv` unless debugging the binding path.
- Loading the temporary SPI test device:
  `sudo modprobe spidev`
  `sudo insmod tools/spi_test_device/spi_dln2_spidev_test.ko bus_num=0 chip_select=0 max_speed_hz=100000`
- If the generated `/dev/spidev0.0` is root-only, use
  `sudo chmod a+rw /dev/spidev0.0` for this temporary lab session.
- Run the SPI fixture test with:
  `./scripts/spi_c3_slave_test.py --c3 /dev/ttyACM2 --spidev /dev/spidev0.0 --length 16 --speed 100000 --mode 0 --direction full`
- Verified SPI fixture matrix: 10 kHz, 100 kHz, 500 kHz, and 1 MHz at transfer
  lengths 1, 4, 16, and 64 bytes all passed.
- After switching both S3 SPI master and C3 SPI slave paths to DMA mode,
  256-byte transfers pass at 100 kHz and 1 MHz. This covers the advertised
  Linux `spi-dln2` maximum transfer size.
- Linux `spi-dln2` applies userspace setup changes by disabling the port,
  setting speed/mode/frame size, then enabling the port again. The S3 firmware
  must apply speed and mode changes to the active ESP-IDF `spi_device_handle_t`;
  updating only DLN-2 shadow state leaves the hardware transaction mode stale.
- `scripts/spi_c3_slave_test.py --direction read` exercises Linux
  `DLN2_SPI_READ`; the C3 should observe zero MOSI bytes while the Linux host
  receives the C3 MISO pattern. `--direction write` exercises
  `DLN2_SPI_WRITE`; the C3 verifies MOSI while host RX is intentionally absent.
- Because DLN-2 controls chip-select separately through `SET_SS` and
  per-transfer attributes, the S3 firmware uses a normal ESP-IDF SPI device
  with `spics_io_num = -1` and manually drives the CS GPIO. For CPOL=1 modes
  2 and 3, the S3 must still make the ESP-IDF SPI master drive SCLK to idle
  high before the first selected transfer. The firmware now sends one dummy
  8-bit transaction with CS inactive after SPI enable/reconfiguration; normal
  selected SPI slaves ignore this clock train because CS is high.
- Without the CPOL idle-prime step, mode 3 could fail on the first short
  full-duplex transaction after a mode change. The C3 fixture observed one
  extra byte and both MOSI/MISO streams were shifted.
- SPI modes 0-3 have passed full-duplex validation at 100 kHz and 1 MHz with
  lengths 1, 2, 16, and 256 bytes. Mode 3 has also passed read-only and
  write-only validation at lengths 1, 2, 16, and 256 bytes.
- Reflashing or unplugging the S3 DLN-2 USB device removes any temporary
  `spidev0.0` child created by `tools/spi_test_device`; reload the helper after
  the new `/sys/class/spi_master/spi0` appears.
- Remove the temporary SPI child after testing with:
  `sudo rmmod spi_dln2_spidev_test`
- Do not use the first-generation `spi_test_device.ko` after a S3 USB
  re-enumeration. It can retain a stale `spi_device` pointer and fault on
  unload. The current replacement helper is `spi_dln2_spidev_test.ko`.

## ESP32-C3 I2C Fixture

- `c3_test` version `c3-probe-v4` adds a command-driven I2C slave fixture while
  preserving the GPIO and SPI fixture commands.
- I2C fixture pins:
  - C3 GPIO0: SDA, connect to S3 GPIO38.
  - C3 GPIO1: SCL, connect to S3 GPIO47.
  - GND: connect C3 and S3 ground.
- The default fixture slave address is `0x52`; `I2C START [addr]` accepts any
  7-bit non-zero address.
- The current test maximum is 256 bytes, matching the S3 DLN-2 I2C firmware's
  maximum transfer size.
- `I2C RX <len>` arms one C3 slave receive before the Linux host write. After
  the host transfer, `I2C STATUS` reports the received bytes in `rx=<hex>`.
- `I2C LOAD <len>` preloads the C3 slave transmit buffer before the Linux host
  read. The transmitted pattern is `c0 c1 ... ff c0 ...`.
- `scripts/i2c_c3_slave_test.py` finds the DLN-2 I2C adapter by looking for an
  `/sys/class/i2c-dev/i2c-X/name` containing `dln2-i2c`, unless `--i2c-dev`
  is provided. On this host the current adapter is `/dev/i2c-4`.
- The script uses the Linux `i2c-dev` `I2C_SLAVE` ioctl plus raw `read()` and
  `write()` calls, so it exercises the mainline `i2c-dln2` `READ` and `WRITE`
  command paths instead of bypassing the kernel driver.
- The script also supports Linux `I2C_RDWR` ioctl register-style tests. It
  builds two `struct i2c_msg` entries in Python with `ctypes`: one write
  message containing the register byte, followed by one read message.
- Mainline Linux `i2c-dln2` does not currently turn `I2C_RDWR` into one DLN-2
  combined-transfer command. Its `master_xfer()` loop sends one DLN-2 command
  per `i2c_msg`, either `DLN2_I2C_WRITE` or `DLN2_I2C_READ`.
- Because of that Linux driver behavior, firmware-side `mem_addr_len` remains
  unused by the current host path. The request fields are still parsed by the
  S3 firmware, but Linux fills `mem_addr_len = 0` for both read and write
  commands.
- The current validated `I2C_RDWR` path is suitable for register-read devices
  that accept a STOP between the register-address write and the data read.
  Devices that require a strict repeated START would need a Linux driver path
  using a DLN-2 combined-transfer command or another custom kernel change.
- The C3 USB-Serial/JTAG path can need a short settling period after opening
  the TTY. The I2C script now waits after open and retries `PING` before
  starting the fixture.
- Current Codex shell processes still do not list group `i2c` in `id`, while
  `/dev/i2c-4` is `root:i2c 660`. Run the script through `sg i2c -c ...`, a
  `newgrp i2c` shell, or `sudo` if direct access fails.
- Short I2C wires may work with the internal pullups currently enabled on the
  S3 master bus, but external pullups around 4.7 kOhm from SDA/SCL to 3V3 are
  the more robust lab setup. Do not pull the bus up to 5 V.
- The C3 fixture has validated Linux `i2c-dln2` transfers at 100 kHz for 1,
  4, 16, 64, and 256 byte raw writes and reads. This covers the current
  firmware maximum transfer length.
- The same fixture has validated two-message Linux `I2C_RDWR` register-style
  reads through `/dev/i2c-4` for register bytes `0x00`, `0x10`, and `0x7f`,
  with 1, 4, 16, 64, and 256 byte reads.
- `scripts/i2c_c3_slave_test.py` now also drives the Linux `I2C_SMBUS` ioctl.
  It uses the UAPI layouts from `linux/i2c-dev.h` and `linux/i2c.h`:
  `struct i2c_smbus_ioctl_data` points at `union i2c_smbus_data`, whose block
  field stores length in `block[0]`.
- Validated SMBus operations:
  - `I2C_SMBUS_BYTE`: receive byte and send byte.
  - `I2C_SMBUS_BYTE_DATA`: command-plus-byte read and write.
  - `I2C_SMBUS_WORD_DATA`: command-plus-little-endian-word read and write.
  - `I2C_SMBUS_I2C_BLOCK_DATA`: command-plus-block read and write up to the
    SMBus 32-byte block limit.
- SMBus block writes observed by the C3 are `command + payload`; the Linux
  I2C block protocol length byte is an ioctl-side value and is not placed on
  the I2C bus for `I2C_SMBUS_I2C_BLOCK_DATA`.
- The current validation intentionally does not claim strict SMBus block read
  or block process call support. Mainline `i2c-dln2` advertises
  `I2C_FUNC_SMBUS_BLOCK_PROC_CALL`, but its transfer function only handles
  fixed-length read messages; variable-length `I2C_M_RECV_LEN` behavior needs
  separate kernel-level scrutiny before relying on it.
- `scripts/i2c_c3_stress_test.py` covers short stability and error recovery
  without changing firmware. It keeps the C3 fixture at `0x52`, deliberately
  targets unused address `0x53`, and verifies a good `0x52` transaction after
  each expected failure.
- Observed host error mapping for bad address `0x53` on the current system:
  raw write and two-message `I2C_RDWR` return `EPROTO`; raw read returns
  `EREMOTEIO`. The exact errno is a Linux/i2c-core path detail, so tests
  should require failure plus recovery rather than one fixed errno.
- A 50-cycle mixed I2C stress pass has validated repeated raw read/write,
  two-message `I2C_RDWR`, and SMBus byte-data operations in one C3 fixture
  session. This is a short functional soak, not yet a long-duration endurance
  test.

## Host Support Package

- Linux automatically creates the DLN-2 GPIO, I2C, SPI controller, and IIO ADC
  children, but it does not create a generic `/dev/spidev*` node for a bare SPI
  controller. The host support package keeps that policy boundary explicit and
  provides a host-side SPI child registrar.
- `scripts/install_host_support.sh` is the intended one-time setup entrypoint.
  It must run as root, builds `tools/spi_test_device/spi_dln2_spidev_test.ko`
  against the running kernel, copies it to `/usr/local/lib/esp_usb_dln2/`, and
  installs udev/systemd support files.
- `host_support/99-esp-usb-dln2.rules.in` matches VID/PID `a257:2013` and
  triggers `esp-usb-dln2-spi.service` on USB add. It also sets permissions for
  DLN-2 GPIO, I2C, IIO, and spidev nodes.
- `scripts/esp_usb_dln2_setup_host_device.sh` is the root-side systemd
  oneshot. It finds a `/sys/class/spi_master/spi*` whose parent driver is
  `dln2-spi`, registers CS0 with the `dh2228fv` modalias accepted by mainline
  `spidev`, and fixes permissions for current DLN-2 nodes.
- The setup script handles the common stale state where
  `spi_dln2_spidev_test` is loaded but `/dev/spidev*` disappeared after S3 USB
  re-enumeration. If the helper is not in use, it unloads and reloads it
  against the current `dln2-spi` master.
- Default access group is `plugdev`, configurable at install time with
  `sudo ./scripts/install_host_support.sh --group GROUP` or afterward through
  `/etc/esp_usb_dln2/host.conf`.
- `scripts/check_device_ready.sh` is the user-facing diagnostic. It checks
  USB presence, GPIO by-id link, DLN-2 I2C adapter, DLN-2 SPI master,
  `/dev/spidev*`, DLN-2 IIO ADC, and basic read/write permissions.

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
- After reserving GPIO10-GPIO13 for SPI, that stage's GPIO map exposed 14
  lines.
  Verified:
  - `gpiodetect` reports `gpiochip2 [dln2] (14 lines)`.
  - `gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 0 1 2 3 4 5 6 7 8 9 10 11 12 13`
  - `gpioget -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 --numeric 0 1 2 3 4 5 6 7 8 9 10 11 12 13`
  - `gpiomon -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -e both --debounce-period 10ms --idle-timeout 1s 13`
  - `/sys/class/spi_master/spi0` resolves to a `dln2-spi` child device.
- After reserving GPIO14-GPIO17 for ADC, the current GPIO map exposes 10
  lines. Verified:
  - `gpiodetect` reports `gpiochip2 [dln2] (10 lines)`.
  - `gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 0 1 2 3 4 5 6 7 8 9`
  - `gpioget -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 --numeric 0 1 2 3 4 5 6 7 8 9`
  - `gpiomon -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -e both --debounce-period 10ms --idle-timeout 1s 9`
  - `/sys/class/spi_master/spi0` still resolves to a `dln2-spi` child device.
  - `/sys/bus/iio/devices/iio:device0/name` reports `dln2-adc`.
  - `in_voltage0_raw` through `in_voltage3_raw` read successfully.

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
  In the current firmware, line 0 maps to ESP32 GPIO1 and line 9 maps to
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
- ESP32-S3 ADC2 is safe here because the firmware does not use Wi-Fi. If Wi-Fi
  is added later, ADC2 access needs to be re-evaluated.
- ADC raw reads create and keep one ESP-IDF oneshot unit. Linux direct sysfs
  reads enable/disable individual ADC channels around each read; the firmware
  claims/releases the corresponding physical GPIO on those channel commands.
- ADC buffered IIO mode now has firmware support for `CHANNEL_SET_CFG`,
  `CONDITION_MET_EV`, and `GET_ALL_VAL`. It was validated with:
  `sudo ./scripts/iio_adc_buffer_test.py --channels "0 1" --frequency 10 --samples 8`.
- The validated buffer test read 8 frames from `/dev/iio:device0`; each frame
  contained both enabled channels. The observed values were full-scale because
  the ADC inputs were floating, so this is protocol validation, not calibration
  or voltage-accuracy validation.
- Root access is still required for the current host's IIO buffer sysfs files
  and `/dev/iio:device0` unless udev/sysfs permissions are relaxed.
