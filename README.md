# ESP USB DLN2

English | [简体中文](README.zh-CN.md)

ESP USB DLN2 turns an ESP32-S3 board into a USB GPIO/I2C/SPI/ADC adapter.
The same firmware supports Linux through mainline DLN-2 drivers and Windows
10/11 through system WinUSB and a Python client. The target board is
**WeAct ESP32S3-A**.

| Host | How applications access the adapter | Start here |
| --- | --- | --- |
| Linux | GPIO chardev, I2C, SPI/spidev, and IIO ADC | [Quick start](#quick-start) below; [host setup details](HOST_SUPPORT.md) |
| Windows 10/11 | Python client using WinUSB, with no third-party Python dependencies | [Windows guide](WINDOWS.md) |

The firmware provides 10 GPIO lines, one I2C port, one SPI port with CS0,
and four ADC channels. All external signals are **3.3 V**. This is lab
firmware; read the [known issues](#known-issues) before connecting hardware.

## Quick Start

1. Check the [requirements](#requirements), then [build and flash](#build-and-flash) the S3.
2. Connect the **native USB** port and follow the [host setup](#one-time-host-setup) for your OS.
3. Check the [pin map](#pin-map), then try the [usage examples](#usage-examples).

If the board already has the current firmware, start at host setup. An
ESP32-C3 is only needed for the optional developer test fixture.

## Requirements

- WeAct ESP32S3-A or a compatible ESP32-S3 board matching the pin map below.
- USB connections for the board's flash/log serial port and its native USB
  device port. The serial port is for flashing; the native port exposes DLN-2.
- Git and **ESP-IDF v5.5.4** to build the firmware. Other ESP-IDF v5.x releases
  have not been validated for this project.

For a Linux host:

- Mainline drivers: `dln2`, `gpio-dln2`, `i2c-dln2`, `spi-dln2`, and `dln2-adc`.
- Host support installer: `make`, a C compiler, headers for the running kernel,
  `kmod`, `systemd`, and `udev`. Install `dkms` for automatic SPI helper rebuilds
  after kernel upgrades.
- Usage and test tools: Python 3, `libgpiod` command-line tools, and `i2c-tools`.
  The GPIO commands below use libgpiod v2 syntax.

For a Windows 10/11 host:

- Python 3.10+; the client uses only the standard library and system WinUSB.
- Current firmware for automatic WinUSB binding. Boards running older firmware
  need the one-time update described in the [Windows guide](WINDOWS.md).

## Build and Flash

Get the source:

```shell
git clone https://github.com/Gdszzy/ESP_USB_DLN2.git
cd ESP_USB_DLN2
```

Run the commands for your platform. Replace the SDK path and example serial
port with your actual values; these commands target the S3, not the C3 fixture.

**Linux / Bash:**

```bash
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash
```

**Windows / an ESP-IDF v5.5.4 PowerShell terminal:**

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash
```

After flashing, connect or replug the S3 **native USB** port. To read firmware
logs, run `idf.py -p <FLASH_PORT> monitor` with the same serial port.

## One-Time Host Setup

### Windows 10/11

The updated firmware automatically binds to system WinUSB. From the project
root, run:

```powershell
py scripts\dln2_windows.py devices
py scripts\dln2_windows.py check
```

`check` reads identity and capabilities without driving pins. Expect hardware
ID `0x00000200`, 10 GPIO lines, and four ADC channels. See the
[Windows guide](WINDOWS.md) for I/O commands, Python usage, and driver checks.

### Linux

Linux binds the DLN-2 drivers automatically. Install host support once to set
normal-user permissions and create the SPI `spidev` node:

```bash
getent group plugdev || sudo groupadd -r plugdev
sudo ./scripts/install_host_support.sh
sudo usermod -aG plugdev "$USER"
newgrp plugdev
esp-usb-dln2-check
```

`newgrp` opens a shell with the new group membership. Ready output should be
all `OK`, including `IIO buffer sysfs is writable`.

Normal reboots and USB reconnections do not require reinstalling host support.
For access-group options, DKMS, a missing `spidev` node, or removal, see
[HOST_SUPPORT.md](HOST_SUPPORT.md).

## Pin Map

GPIO commands on both platforms use **DLN-2 line offsets**, not ESP32-S3
GPIO numbers: line 0 is GPIO1, line 1 is GPIO2, and so on. ADC channels 0–3
map to GPIO14–17 in the same order.

| Function | Channel | ESP32-S3 pins |
| --- | --- | --- |
| GPIO | line offsets 0–9 | GPIO1, GPIO2, GPIO4–GPIO9, GPIO18, GPIO21 |
| I2C | port 0 | SDA GPIO38, SCL GPIO47 |
| SPI | port 0, CS0 | SCLK GPIO10, MOSI GPIO11, MISO GPIO12, CS GPIO13 |
| ADC | channels 0–3 | GPIO14–GPIO17 |
| Native USB | DLN-2 device | GPIO19/GPIO20 |

Use a common ground and 3.3 V signals. Reserved pins include boot strapping,
native USB, UART0, PSRAM, JTAG, and the onboard LED. See
[TECH_NOTES.md](TECH_NOTES.md) for the exclusion list and
[Notes and safety](#notes-and-safety) before wiring.

## Usage Examples

The commands below are for **Linux**. For Windows I/O commands, see the
[Windows guide](WINDOWS.md#使用). Run output commands only after checking the wiring.

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

Replace `<BUS_NUMBER>` with the bus whose name starts with `dln2-i2c-`,
then scan it with read-byte probing:

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
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3" --skip-buffer
```

## Application Examples

The optional [OLED dashboard](examples/oled_flash_button_dashboard/README.md)
uses an SSD1306 display, W25Q-style SPI flash, AHT20/BMP280-class sensors, and
four buttons. It uses the adapter's Linux GPIO/I2C/SPI interfaces. Wiring and
run commands are in the example README.

## Current Status

The Linux column summarizes earlier hardware validation. The Windows column
covers the WinUSB adaptation validated on 2026-10-19.

| Function | Linux validation | Windows validation |
| --- | --- | --- |
| USB | Automatic `dln2` binding | Automatic system WinUSB binding, identity reads, and re-enumeration |
| GPIO | Read/write and IRQ events | Input reads, output latch/direction, and release; no voltage measurement |
| I2C | Raw transfers, two-message `I2C_RDWR`, SMBus paths, and error recovery | AHT20 + BMP280 measurements with AHT20 CRC checks, plus NACK recovery |
| SPI | SPI master and host-created `spidev` access | Modes 0–3 and transfer boundaries; no attached slave data verification |
| ADC | Raw and buffered reads, four-channel known-voltage smoke | Raw range checks and asynchronous events; no accuracy calibration |

Windows and Ubuntu WSL each pass 24 host protocol tests. **Linux USB binding
has not been rerun on hardware after adding the WinUSB descriptors.** WSL
protocol tests do not exercise Linux device drivers. See the detailed
[Windows validation record](WINDOWS.md#本次验证记录2026-10-19).

## Development

### Where to change code

| Path | Responsibility |
| --- | --- |
| [main/](main/) | USB descriptors, request framing, and event transmission |
| [components/dln2/](components/dln2/) | DLN-2 commands, GPIO/I2C/SPI/ADC implementations, and pin ownership |
| [scripts/dln2_windows.py](scripts/dln2_windows.py) | Windows discovery, WinUSB transport, Python API, and CLI |
| [host_support/](host_support/) / [tools/spi_test_device/](tools/spi_test_device/) | Linux permissions, plug-in setup, and SPI child registration |
| [tests/](tests/) | Host protocol tests, compiled USB descriptor checks, and Windows hardware smoke |
| [c3_test/](c3_test/) | Optional ESP32-C3 GPIO/I2C/SPI closed-loop fixture |

The device uses one full-speed vendor interface, bulk OUT `0x01` and IN
`0x81`, VID/PID `a257:2013`, and serial `12345678`. GPIO IRQ and ADC events
share the USB event path. Protocol details and implementation decisions are in
[TECH_NOTES.md](TECH_NOTES.md).

### Tests without hardware

Run from the project root:

```bash
python3 -B -m unittest discover -s tests -v
```

On Windows, use `py -B -m unittest discover -s tests -v`.

### Basic Smoke Test

On Linux, after host setup, these checks need no external peripherals:

```bash
esp-usb-dln2-check
gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00
./scripts/iio_adc_smoke_test.py --channels "0 1 2 3"
```

They check device access, GPIO metadata, and raw/buffered ADC paths, not
measurement accuracy. I2C/SPI transfers and GPIO IRQ checks need external
hardware.

For compiled descriptor checks and the Windows hardware smoke, follow
[Windows development checks](WINDOWS.md#开发检查); the smoke drives GPIO and
requires a bare board. For the Linux full regression, wire the
[C3 fixture](c3_test/README.md) before running
[scripts/full_regression.sh](scripts/full_regression.sh).

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

- Linux `spidev` access uses an experimental host-side helper module. DKMS
  handles normal kernel upgrades; `--dkms no` installs need the installer
  rerun after an upgrade. See [host support](HOST_SUPPORT.md).
- I2C writes and reads are separate transfers. Linux `i2c-dln2` sends one DLN-2
  transfer per `i2c_msg`; the Windows client also uses separate write/read
  operations. Devices requiring repeated START need separate validation.
- Absolute ADC accuracy and calibration are not validated.
- Long-duration GPIO IRQ, I2C, SPI, and ADC endurance tests are still pending.
- Only the DLN-2 command subset used by these host tools is implemented.
  Unsupported commands return explicit errors.

## Feedback Data

For a Windows report, include the Windows/Python versions and the output of
`py scripts\dln2_windows.py devices` and `py scripts\dln2_windows.py check`.

On Linux, include:

```bash
uname -a
lsusb -d a257:2013
esp-usb-dln2-check
journalctl -u esp-usb-dln2-spi.service -n 80 --no-pager
dmesg | grep -iE 'dln2|a257|spidev|iio'
```

## Documentation Map

- [WINDOWS.md](WINDOWS.md): Windows setup, CLI/Python usage, driver checks, and validation.
- [HOST_SUPPORT.md](HOST_SUPPORT.md): Linux installation, udev/systemd, and the SPI helper.
- [TECH_NOTES.md](TECH_NOTES.md): protocol details, pin decisions, and bring-up findings.
- [OLED dashboard](examples/oled_flash_button_dashboard/README.md): Linux application and wiring example.
- [C3 test fixture](c3_test/README.md): optional closed-loop hardware regression.
- [PROGRESS.md](PROGRESS.md): historical milestones; use the guides above for current setup.

## License

The main firmware, host scripts, and documentation are licensed under the
Apache License 2.0. See [LICENSE](LICENSE).

The Linux kernel helper in `tools/spi_test_device/` is a Linux kernel module
and remains licensed under GPL-2.0 as declared by its SPDX header.
