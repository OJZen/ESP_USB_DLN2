# Progress and Milestones

This file tracks implementation progress for the ESP32-S3 DLN-2 compatible
USB GPIO device.

## Milestones

- [x] M0: Initialize git repository and ignore generated build artifacts.
- [x] M1: Confirm Linux DLN-2 protocol details used by `dln2.ko` and
      `gpio-dln2.ko`.
- [x] M2: Bring up ESP-IDF TinyUSB vendor-specific bulk interface.
- [x] M3: Implement DLN-2 control commands required by Linux enumeration.
- [x] M4: Implement GPIO-only command subset and ESP32-S3 pin mapping.
- [x] M5: Build, flash to `/dev/ttyACM1`, and verify Linux creates
      `/dev/gpiochipX`.
- [x] M6: Implement DLN-2 GPIO event configuration and verify Linux `gpiomon`
      can enable edge monitoring.
- [x] M7: Expand the stable GPIO export while excluding occupied and high-risk
      ESP32-S3 pins.
- [x] M8: Replace GPIO event polling with ESP GPIO interrupt driven event
      queuing.
- [x] M9: Add an ESP32-C3 based external GPIO probe firmware and host
      closed-loop test harness.
- [x] M10: Validate the C3 closed-loop probe against the S3 DLN-2 GPIO device.
- [x] M11: Harden DLN-2 USB bulk OUT request framing.
- [x] M12: Reduce output-direction switching glitches.
- [x] M13: Derive the GPIO IRQ, I2C, SPI, and ADC implementation plan from
      Linux DLN-2 subdrivers and the `pico-usb-io-board` device reference.
- [x] M14: Add central ESP32-S3 pin ownership tracking and route GPIO
      enable/disable through it.
- [x] M15: Implement the Linux `i2c-dln2` minimal enable/read/write/disable
      path and verify adapter enumeration.
- [x] M16: Implement the Linux `spi-dln2` minimal controller probe path and
      register a SPI master on Linux.

## Log

### 2026-06-05

- Connected the ESP32-C3 probe on `/dev/ttyACM2`, identified as an Espressif
  USB JTAG/serial debug unit.
- Flashed `c3_test` to `/dev/ttyACM2`; esptool identified an ESP32-C3 rev v0.4
  with MAC `cc:8d:a2:c2:88:2c`.
- Found that the initial C3 firmware printed boot/probe logs over
  USB-Serial/JTAG but did not accept host-to-device console input. Fixed this
  by configuring the C3 project for USB-Serial/JTAG console and explicitly
  installing the USB-Serial/JTAG driver plus VFS in the probe firmware.
- Added `c3_test/sdkconfig.defaults` with
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` so regenerated C3 configs keep the
  correct console path.
- Increased the C3 host script serial write timeout so USB CDC writes have
  enough time during board reset and driver startup.
- Updated the C3 host script to open the serial port exclusively and to run the
  `gpiomon` event check on every selected C3/S3 channel pair.
- User wired C3 channels 0-6 only; channel 7 / C3 GPIO10 was intentionally left
  disconnected.
- Ran the closed-loop host test:
  `python3 -u scripts/c3_closed_loop_test.py --c3-port /dev/ttyACM2 --lines "0 1 2 3 4 5 6"`.
- Verified all seven wired channels passed:
  - S3 output through Linux `gpiod`, observed by C3 input.
  - C3 output, observed by S3 input through Linux `gpiod`.
  - C3 pulse generation on each selected channel, observed by Linux `gpiomon`
    edge events.
- Hardened the S3 USB vendor OUT path so it accumulates DLN-2 request frames by
  the header `size` field instead of treating one TinyUSB RX callback as one
  complete request.
- Rebuilt and flashed the S3 firmware with the framed RX path to `/dev/ttyACM1`.
- Verified after flashing:
  - `lsusb -d a257:2013` reports `Diolan DLN-2 USB GPIO`.
  - `gpiodetect` reports `gpiochip2 [dln2] (18 lines)`.
  - `gpioinfo -c ... 0..17` reports all 18 exported lines as inputs.
  - `gpioget -c ... --numeric 0..17` succeeds.
  - The C3 closed-loop test on S3 lines 0-6 passes S3-output, C3-output, and
    per-line `gpiomon` event checks.
- Updated GPIO output direction handling so `PIN_SET_DIRECTION` applies the
  stored output value before enabling output mode and confirms it afterward.
  This reduces the chance of a transient wrong level when userspace switches a
  line from input to output.
- Rebuilt and flashed the updated S3 firmware to `/dev/ttyACM1`.
- Verified `gpioget`, `gpioset`, and the C3 closed-loop test on S3 lines 0-6
  still pass.
- Added `DLN2_IMPLEMENTATION_PLAN.md` to make the next feature phase
  source-driven. The plan records the Linux command paths for GPIO IRQ
  hardening, I2C, SPI, and ADC, and uses `notro/pico-usb-io-board` as the
  device-side reference for module dispatch and pin ownership.
- Confirmed the next required architectural step is a central ESP32-S3 pin
  ownership layer before enabling I2C/SPI/ADC. Linux probes the GPIO subdriver
  and enables all exported GPIO lines, so peripheral pins must either be
  removed from the exported GPIO map or rejected cleanly as owned by another
  module.
- Added `components/dln2/dln2_pin.c`, a central ESP32-S3 physical pin ownership
  table modeled after the `pico-usb-io-board` approach.
- The ownership table marks the known occupied/high-risk pins unavailable and
  supports module owners for GPIO, I2C, SPI, and ADC.
- Updated `DLN2_GPIO_PIN_ENABLE` and `DLN2_GPIO_PIN_DISABLE` so GPIO lines claim
  and release their mapped physical ESP32-S3 GPIO pins.
- Rebuilt and flashed the S3 firmware to `/dev/ttyACM1`.
- Verified after the pin ownership change:
  - `lsusb -d a257:2013` reports `Diolan DLN-2 USB GPIO`.
  - `gpiodetect` reports `gpiochip2 [dln2] (18 lines)`.
  - `gpioinfo` reports all 18 lines as inputs.
  - `gpioget` on lines 0-17 succeeds.
  - `gpioset` on line 17 succeeds.
  - `gpiomon` can request events on line 17.
  - The C3 closed-loop test still passes for wired lines 0-6.
- Implemented the minimal Linux `i2c-dln2` command set:
  `DLN2_I2C_ENABLE`, `DLN2_I2C_DISABLE`, `DLN2_I2C_WRITE`, and
  `DLN2_I2C_READ`.
- Assigned I2C port 0 to ESP32-S3 GPIO38 as SDA and GPIO47 as SCL. These pins
  are exposed on the WeAct ESP32S3-A header and were not part of the current
  18-line GPIO export, so GPIO line count remains unchanged.
- I2C enable now claims SDA/SCL through the central pin ownership table and
  initializes ESP-IDF's I2C master bus at 100 kHz with internal pullups enabled.
- Rebuilt and flashed the S3 firmware to `/dev/ttyACM1`.
- Verified after the I2C change:
  - `lsusb -d a257:2013` reports `Diolan DLN-2 USB GPIO`.
  - `gpiodetect` still reports `gpiochip2 [dln2] (18 lines)`.
  - `i2cdetect -l` now reports `/dev/i2c-4` as
    `dln2-i2c-3-2.1.2:1.0-0`.
  - `/sys/class/i2c-dev/i2c-4/name` reports the same `dln2-i2c` adapter name.
  - `gpioget` on lines 0-17 succeeds.
  - The C3 closed-loop test still passes for wired lines 0-6.
- After adding user `jzen` to the host `i2c` group, verified I2C runtime access
  through `newgrp i2c`.
- `i2cdetect -F 4` reports the Linux `dln2-i2c` adapter capability set.
- `i2cdetect -r -y 4` completes a full read-byte scan of `/dev/i2c-4`; no
  devices are present on the currently empty bus, so all probed addresses
  report `--`.
- Implemented the minimal Linux `spi-dln2` command set for controller probe and
  basic runtime transfer calls.
- Assigned SPI port 0 to ESP32-S3 GPIO10 as SCLK, GPIO11 as MOSI, GPIO12 as
  MISO, and GPIO13 as CS0.
- Removed GPIO10-GPIO13 from the exported GPIO line map so `gpio-dln2` does not
  claim those physical pins before `spi-dln2` probes. The exported GPIO chip
  now has 14 lines.
- Rebuilt and flashed the S3 firmware to `/dev/ttyACM1`.
- Verified after the SPI change:
  - `lsusb -d a257:2013` reports `Diolan DLN-2 USB GPIO`.
  - `gpiodetect` reports `gpiochip2 [dln2] (14 lines)`.
  - `i2cdetect -l` still reports `/dev/i2c-4` as `dln2-i2c-3-2.1.2:1.0-0`.
  - `/sys/class/spi_master/spi0` exists and resolves to the DLN-2 SPI child
    device path `.../dln2-spi.3.auto/spi_master/spi0`.
  - `spi_dln2` is loaded and bound through the parent `dln2` MFD driver.
  - `gpioinfo` and `gpioget` work on current GPIO lines 0-13.
  - `gpiomon` can request events on current GPIO line 13.
  - The C3 closed-loop test still passes for wired lines 0-6.
- SPI transfer validation is still pending because the kernel registers only a
  SPI master by default; no `/dev/spidev*` device is created without a described
  SPI slave device. The next validation target is a C3 SPI slave fixture or a
  known external SPI device.

### 2026-06-04

- Read `doc.md`; project target is ESP32-S3 firmware emulating a DLN-2 USB
  GPIO controller for Linux `dln2` and `gpio-dln2`.
- Confirmed current firmware entry point is an empty `app_main()`.
- Initialized this git repository.
- Added `.gitignore` to keep generated ESP-IDF build output out of git.
- Added `espressif/esp_tinyusb` dependency through `main/idf_component.yml`.
- Implemented a single-interface TinyUSB vendor device using VID/PID
  `a257:2013`.
- Implemented DLN-2 request parsing, response packing, control commands
  `GET_DEVICE_VER` and `GET_DEVICE_SN`, and GPIO commands needed by the Linux
  `gpio-dln2` driver.
- GPIO MVP exposes 8 lines mapped to ESP32-S3 GPIO1, GPIO2, GPIO4, GPIO5,
  GPIO6, GPIO7, GPIO8, and GPIO9.
- Built successfully with ESP-IDF v5.5.4 and flashed to `/dev/ttyACM1`.
- Linux enumerated the board as `Bus 001 Device 024: ID a257:2013 Diolan
  DLN-2 USB GPIO`.
- Kernel autoloaded `dln2`, `gpio_dln2`, `i2c_dln2`, `spi_dln2`, and
  `dln2_adc`.
- Verified GPIO chardev:
  `/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00 -> ../../gpiochip2`.
- Verified chardev chip info with a temporary ioctl probe:
  `chip name=gpiochip2 label=dln2 lines=8`.
- Verified line 0 input read path through GPIO chardev:
  `line 0 input value=1`.
- After installing libgpiod v2.2.4, verified the normal tools:
  - `gpiodetect` reports `gpiochip2 [dln2] (8 lines)`.
  - `gpioinfo -c /dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00`
    reports 8 unnamed input lines.
  - `gpioget -c ... --numeric 0 1 2 3 4 5 6 7` succeeds.
  - `gpioset -c ... -t 100ms,0 0=1` succeeds and exits.
- Added `scripts/gpio_pin_walk.sh` for meter probing. It now toggles all
  selected lines together, logs every high/low transition, and drives them low
  before release when stopped.
- Added line-to-ESP32-GPIO mapping labels to the meter probing script logs.
- Implemented GPIO event support for Linux `gpio-dln2`:
  `SET_EVENT_CFG`, `GET_EVENT_CFG`, event packet generation on handle 0, and
  debounce minimum event interval handling.
- Added a FreeRTOS event polling task that sends `DLN2_GPIO_CONDITION_MET_EV`
  packets over the vendor bulk IN endpoint without using GPIO loopback wiring.
- Rebuilt and flashed the updated firmware to `/dev/ttyACM1`.
- Verified after flashing:
  - `lsusb -d a257:2013` reports `Diolan DLN-2 USB GPIO`.
  - `gpiodetect` reports `gpiochip2 [dln2] (8 lines)`.
  - `gpioget -c ... --numeric 0 1 2 3 4 5 6 7` succeeds.
  - `gpiomon -c ... -e both --debounce-period 10ms --idle-timeout 1s 0`
    successfully enables edge monitoring and exits cleanly.
- Expanded the exported GPIO set from 8 to 18 lines, mapped to ESP32-S3
  GPIO1, GPIO2, GPIO4-GPIO18, and GPIO21.
- Documented excluded occupied/high-risk pins: boot/JTAG strapping pins,
  native USB pins, UART0 flash/log pins, OSPI PSRAM pins, onboard key/LED
  paths, and reserved high-numbered alternate-function pins.
- Rebuilt and flashed the 18-line firmware. Verified:
  - `gpiodetect` reports `gpiochip2 [dln2] (18 lines)`.
  - `gpioinfo -c ... 0..17` reports all 18 exported lines as inputs.
  - `gpioget -c ... --numeric 0..17` succeeds.
  - `gpioset -c ... -t 100ms,0 17=1` succeeds and releases back to input.
  - `gpiomon -c ... -e both --debounce-period 10ms --idle-timeout 1s 17`
    successfully enables event monitoring on the new highest line.
- Replaced the first GPIO event polling implementation with ESP GPIO ISR
  handlers feeding a FreeRTOS queue. The USB event task now waits on queued
  edge events and sends DLN-2 event packets from task context.
- Rebuilt and flashed the ISR queue event firmware. Verified `gpioinfo`,
  `gpioget`, `gpioset`, and `gpiomon` still work on the 18-line chip.
- Added `c3_test`, an ESP32-C3 firmware project that exposes 8 serial-controlled
  GPIO probe channels. The default C3 channel GPIOs are GPIO0, GPIO1, GPIO3,
  GPIO4, GPIO5, GPIO6, GPIO7, and GPIO10.
- Added `scripts/c3_closed_loop_test.py` to coordinate C3 serial commands with
  S3 libgpiod operations. It tests S3-output-to-C3-input,
  C3-output-to-S3-input, and C3-pulse-to-S3-`gpiomon` event paths.
- Added startup synchronization retries to the C3 host script so it tolerates
  boards that reset when the serial port is opened.
- Built `c3_test` successfully for target `esp32c3`.
