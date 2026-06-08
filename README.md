# ESP USB DLN2

[English](README.md) | [简体中文](README.zh-CN.md)

ESP USB DLN2 turns an ESP32-S3 board into a Linux-compatible DLN-2 style USB
I/O adapter. After flashing the firmware and installing the optional host
support package, Linux can use the S3 through existing mainline drivers instead
of a custom host driver.

Current target board: WeAct ESP32S3-A. Current Linux compatibility target:
mainline `dln2`, `gpio-dln2`, `i2c-dln2`, `spi-dln2`, and `dln2-adc`.

## Quick Start

The paths and TTY names below are examples. Replace `IDF_PATH` and the S3 flash
port with the values on your host.

```bash
git clone https://github.com/Gdszzy/ESP_USB_DLN2.git
cd ESP_USB_DLN2

export IDF_PATH=/path/to/esp-idf
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash
```

After flashing, connect or replug the S3 native USB port, then install the
Linux host support:

```bash
getent group plugdev || sudo groupadd -r plugdev
sudo ./scripts/install_host_support.sh
sudo usermod -aG plugdev "$USER"
newgrp plugdev
esp-usb-dln2-check
```

When `esp-usb-dln2-check` reports all `OK`, basic GPIO/I2C/SPI/ADC access from
Linux is ready. No additional board is required for normal use.

## Current Status

Validated on the current development host:

- USB enumeration with VID/PID `a257:2013`; Linux binds the mainline `dln2`
  MFD driver automatically.
- GPIO chardev through `gpio-dln2`, including line get/set and IRQ events.
- I2C adapter through `i2c-dln2`, including raw read/write, two-message
  `I2C_RDWR` register-style reads, common SMBus ioctl paths, and short error
  recovery stress.
- SPI master through `spi-dln2`, with a host-side helper that creates a lab
  `spidev` child for userspace transfers.
- ADC IIO device through `dln2-adc`, including raw reads, buffered reads, and
  a 4-channel known-voltage electrical smoke test.
- One-time host support install that makes GPIO/I2C/SPI/IIO usable by the
  normal user on the current host.

The project is still a lab/bring-up firmware, not a finished product. See
`Known Issues` and `Notes and Safety` below before wiring external hardware.

## Requirements

Hardware:

- WeAct ESP32S3-A or a compatible ESP32-S3 board with native USB wired to the
  host.
- One USB connection for flashing/logging and one native USB connection for the
  DLN-2 emulated device.

Software:

- ESP-IDF v5.5.4 is the tested SDK version. Other ESP-IDF v5.x releases may
  work, but are not the current validation target.
- Linux with mainline DLN-2 drivers available: `dln2`, `gpio-dln2`,
  `i2c-dln2`, `spi-dln2`, and `dln2-adc`.
- Host build and device tools: `git`, Python 3, `make`, a C compiler, kernel
  headers for the running kernel, `kmod`, `systemd`, and `udev`.
- Recommended user tools: `libgpiod` command-line tools, `i2c-tools`, and
  `dkms`.

## Implemented Device Shape

- USB 2.0 full-speed vendor interface.
- Bulk OUT endpoint `0x01`, bulk IN endpoint `0x81`.
- Manufacturer string `Diolan`, product string `DLN-2 USB GPIO`, serial
  `12345678`.
- DLN-2 packet parser with request framing over TinyUSB vendor endpoints.
- DLN-2 control commands required by Linux MFD probing.
- DLN-2 GPIO, I2C, SPI, and ADC command subsets used by the current Linux
  drivers.
- Shared event path for GPIO IRQ events and ADC triggered-buffer events.
- ESP32-S3 physical pin ownership layer to avoid GPIO/I2C/SPI/ADC conflicts.

## Pin Map

Linux sees DLN-2 GPIO line offsets, not ESP32-S3 GPIO numbers.
The GPIO pins below are ordered by Linux line offset: offset 0 is GPIO1,
offset 1 is GPIO2, and so on.

| Function | Linux name | ESP32-S3 pins |
| --- | --- | --- |
| GPIO lines | offsets 0-9 | GPIO1, GPIO2, GPIO4-GPIO9, GPIO18, GPIO21 |
| I2C port 0 | `/dev/i2c-X`, `dln2-i2c-*` | SDA GPIO38, SCL GPIO47 |
| SPI port 0 | `/sys/class/spi_master/spiX` | SCLK GPIO10, MOSI GPIO11, MISO GPIO12, CS0 GPIO13 |
| ADC port 0 | `/dev/iio:deviceX`, `dln2-adc` | GPIO14-GPIO17 |
| Native USB | DLN-2 USB device | GPIO19/GPIO20 |

Reserved or excluded pins include ESP32-S3 strapping pins, native USB pins,
UART0 flash/log pins, PSRAM pins, JTAG pins, and the onboard LED path. See
`TECH_NOTES.md` for the full exclusion list.

## Build and Flash

ESP-IDF v5.5.4 is the tested environment:

```bash
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

If `IDF_PATH` is not set, source the export script from your ESP-IDF checkout,
for example `. /path/to/esp-idf/export.sh`. After flashing, unplug/replug the
S3 native USB connection if Linux does not immediately bind the DLN-2 device.

## One-Time Host Setup

Linux creates GPIO, I2C, SPI master, and IIO ADC child devices automatically.
It does not create a generic `/dev/spidev*` node for a bare SPI controller, and
default permissions are usually root-only. Install the host support package
once:

```bash
getent group plugdev || sudo groupadd -r plugdev
sudo ./scripts/install_host_support.sh
esp-usb-dln2-check
```

The installer provides:

- udev permissions for DLN-2 GPIO, I2C, IIO, and spidev nodes.
- a systemd oneshot that handles replug/re-enumeration.
- a small host-side kernel helper that registers a temporary `spidev` child on
  the DLN-2 SPI controller. When `dkms` is available, this helper is installed
  as a DKMS package and can rebuild automatically after kernel upgrades.
- `esp-usb-dln2-check`, a readiness diagnostic.
- `esp-usb-dln2-uninstall`, a host support removal command.

If you prefer an existing access group, pass it explicitly, for example
`sudo ./scripts/install_host_support.sh --group i2c`. If your user is not in
the chosen group, add it and start a new login shell:

```bash
sudo usermod -aG plugdev "$USER"
newgrp plugdev
esp-usb-dln2-check
```

Expected ready output is all `OK`, including `IIO buffer sysfs is writable`.
See `HOST_SUPPORT.md` for install details and troubleshooting.

## Usage Examples

Find the GPIO chip:

```bash
gpiodetect
gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00
```

Read and drive GPIO lines:

```bash
gpioget -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 --numeric 0 1 2
gpioset -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -t 100ms,0 0=1
gpiomon -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -e both 0
```

Find the I2C adapter:

```bash
for d in /sys/class/i2c-dev/i2c-*; do
    printf '%s: ' "$d"
    cat "$d/name"
done
```

Scan the DLN-2 I2C bus with read-byte probing:

```bash
i2cdetect -r -y <BUS_NUMBER>
```

Use SPI through the host-created `spidev` node:

```bash
esp-usb-dln2-check
ls -l /dev/spidev*
```

Read ADC raw values:

```bash
cat /sys/bus/iio/devices/iio:device*/name
cat /sys/bus/iio/devices/iio:device*/in_voltage0_raw
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3"
```

## Basic Smoke Test

After host support is installed, these checks do not require extra hardware:

```bash
esp-usb-dln2-check
gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3"
```

If GPIO lines are not connected to external hardware, you can also drive a line
briefly:

```bash
gpioset -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -t 100ms,0 0=1
```

I2C and SPI transfer testing requires the user to connect an actual I2C/SPI
target device to the S3 pins listed in the pin map.

## Notes and Safety

- The firmware intentionally uses VID/PID `a257:2013` to bind Linux's mainline
  DLN-2 driver. Do not ship a product with this VID/PID.
- All GPIO/I2C/SPI/ADC signals are 3.3 V only. Do not connect 5 V signals.
- Do not directly loop GPIO outputs together. A wrong output configuration can
  short two push-pull drivers together. Use series resistors or proper external
  protection when connecting GPIO lines to other hardware.
- GPIO0, GPIO3, GPIO45, and GPIO46 are strapping-sensitive on ESP32-S3 boards.
  This firmware keeps them out of the exported GPIO map.
- GPIO19/GPIO20 are native USB pins and must stay reserved for the DLN-2 USB
  device.
- I2C uses S3 internal pullups in firmware, but external 3.3 V pullups around
  4.7 kOhm are the recommended lab setup.
- ADC currently uses ESP32-S3 ADC2. This is acceptable because this firmware
  does not use Wi-Fi. Revisit ADC2 if Wi-Fi is added.
- Raw ADC values are 10-bit command-path readings, not calibrated voltage
  measurements.

## Known Issues

- SPI userspace access still needs the host support package because Linux does
  not create `spidev` for a bare SPI controller.
- The host-side SPI child registrar is still a lab helper kernel module. DKMS
  covers normal kernel upgrades when available; systems installed with
  `--dkms no` need the installer rerun after a kernel upgrade.
- I2C `I2C_RDWR` validation covers Linux's current `i2c-dln2` behavior, which
  sends one DLN-2 transfer per `i2c_msg`. Devices requiring a strict repeated
  START need separate scrutiny.
- ADC electrical smoke has been validated on all 4 channels with known input
  voltages. Absolute accuracy and calibration are not validated.
- Long-duration GPIO IRQ, I2C, SPI, and ADC endurance tests are still pending.
- The no-extra-hardware smoke test verifies enumeration, permissions, GPIO
  metadata, and ADC IIO paths. I2C/SPI electrical transfers and GPIO IRQs need
  external target hardware to validate end-to-end.
- Only one I2C port, one SPI port with one chip select, and one 4-channel ADC
  port are implemented.
- Unsupported DLN-2 commands intentionally return explicit errors.

## Feedback Data

When sharing test results or reporting a failure, include:

```bash
uname -a
lsusb -d a257:2013
esp-usb-dln2-check
journalctl -u esp-usb-dln2-spi.service -n 80 --no-pager
dmesg | grep -iE 'dln2|a257|spidev|iio'
```

## Documentation Map

- `HOST_SUPPORT.md`: host install, udev/systemd, and SPI spidev helper details.
- `DLN2_IMPLEMENTATION_PLAN.md`: Linux-driver-derived command coverage plan and
  module status.
- `TECH_NOTES.md`: protocol notes, pin decisions, verification notes, and
  pitfalls found during bring-up.
- `PROGRESS.md`: chronological milestone log.
- `c3_test/README.md`: developer-only ESP32-C3 closed-loop fixture notes; not
  needed for normal S3 adapter use.
