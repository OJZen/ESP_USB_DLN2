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
- [x] M17: Implement the Linux `dln2-adc` minimal raw-read path and verify IIO
      sysfs raw reads.
- [x] M18: Add the firmware-side Linux `dln2-adc` triggered-buffer event path
      and host buffer test script.
- [x] M19: Verify Linux IIO buffered ADC sample reads through `/dev/iio:device0`.
- [x] M20: Add an ESP32-C3 SPI slave fixture and Linux spidev-based host test
      tooling.
- [x] M21: Verify DLN-2 SPI full-duplex transfers against the C3 fixture.
- [x] M22: Verify full-size 256-byte DLN-2 SPI transfers through the DMA SPI
      firmware path.
- [x] M23: Verify Linux `spi-dln2` read-only/write-only paths and SPI modes
      0-2 against the C3 fixture.
- [ ] M24: Resolve or explicitly scope the remaining SPI mode-3 behavior.

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
- Implemented the minimal Linux `dln2-adc` raw-read command set:
  `DLN2_ADC_SET_RESOLUTION`, `DLN2_ADC_GET_CHANNEL_COUNT`,
  `DLN2_ADC_CHANNEL_ENABLE`, `DLN2_ADC_ENABLE`,
  `DLN2_ADC_CHANNEL_GET_VAL`, `DLN2_ADC_DISABLE`, and
  `DLN2_ADC_CHANNEL_DISABLE`.
- Assigned ADC port 0 to ESP32-S3 ADC2 inputs GPIO14-GPIO17, exposed to Linux
  as IIO channels `in_voltage0_raw` through `in_voltage3_raw`.
- Removed GPIO14-GPIO17 from the exported GPIO line map so `gpio-dln2` does
  not claim those physical pins before `dln2-adc` raw reads. The exported GPIO
  chip now has 10 lines.
- Rebuilt and flashed the S3 firmware to `/dev/ttyACM1`.
- Verified after the ADC change:
  - `lsusb -d a257:2013` reports `Diolan DLN-2 USB GPIO`.
  - `gpiodetect` reports `gpiochip2 [dln2] (10 lines)`.
  - `i2cdetect -l` still reports `/dev/i2c-4` as `dln2-i2c-3-2.1.2:1.0-0`.
  - `/sys/class/spi_master/spi0` still resolves to the DLN-2 SPI child path.
  - `/sys/bus/iio/devices/iio:device0/name` reports `dln2-adc`.
  - `/sys/bus/iio/devices/iio:device0/in_voltage0_raw` through
    `in_voltage3_raw` read successfully. Current values are from floating
    inputs and are only a protocol-path check.
  - `gpioinfo`, `gpioget`, and `gpiomon` work on current GPIO lines 0-9.
  - The C3 closed-loop test still passes for wired GPIO lines 0-6.
- I2C runtime access still requires a process with group `i2c`. The current
  Codex shell does not list `i2c` in `id`, but both `sg i2c -c ...` and
  `newgrp i2c` successfully run `i2cdetect -F 4` and `i2cdetect -r -y 4`.
- Implemented the firmware-side Linux `dln2-adc` triggered-buffer path:
  `DLN2_ADC_CHANNEL_SET_CFG`, periodic `DLN2_ADC_CONDITION_MET_EV` generation,
  and event dispatch interleaving with GPIO events.
- Added `scripts/iio_adc_buffer_test.py` to configure IIO scan elements,
  set `sampling_frequency`, enable the kernel buffer, and read binary samples
  from `/dev/iio:deviceX`.
- Rebuilt and flashed the S3 firmware to `/dev/ttyACM1`.
- Verified after the ADC buffer firmware change:
  - `lsusb -d a257:2013` reports `Diolan DLN-2 USB GPIO`.
  - `gpiodetect` still reports `gpiochip2 [dln2] (10 lines)`.
  - `i2cdetect -l` still reports `/dev/i2c-4` as `dln2-i2c-3-2.1.2:1.0-0`.
  - `/sys/class/spi_master/spi0` still resolves to the DLN-2 SPI child path.
  - `/sys/bus/iio/devices/iio:device0/name` reports `dln2-adc`.
  - `in_voltage0_raw` through `in_voltage3_raw` still read successfully.
- User verified full IIO buffered reads with root privileges:
  `sudo ./scripts/iio_adc_buffer_test.py --channels "0 1" --frequency 10 --samples 8`.
- The root buffer test returned 8 samples from `/dev/iio:device0` with both
  selected channels present in every frame:
  `in_voltage0=1023` and `in_voltage1=1023`. The inputs are still floating, so
  this validates the Linux IIO buffer protocol path rather than analog accuracy.
- Extended the ESP32-C3 test firmware to version `c3-probe-v2`. It keeps the
  existing GPIO probe commands and adds `SPI START`, `SPI STOP`, `SPI STATUS`,
  and `SPI XFER <len> [timeout_ms]`.
- The C3 SPI fixture uses DMA mode so it can cover the Linux `spi-dln2` maximum
  256-byte transfer size instead of stopping at the non-DMA 64-byte FIFO limit.
- C3 SPI fixture wiring uses C3 GPIO4/5/6/7 as SCLK/MOSI/MISO/CS with the S3
  SPI pins GPIO10/11/12/13. C3 GPIO10 remains unused in the user's current
  fixture.
- Added `scripts/spi_c3_slave_test.py`, which arms one C3 SPI slave transfer,
  sends one Linux `spidev` full-duplex transaction, then verifies both host RX
  and C3-observed MOSI bytes.
- Added `tools/spi_test_device`, a temporary out-of-tree kernel helper that
  registers a `spidev` child on the existing Linux DLN-2 SPI master for lab
  testing. This is needed because Linux only exposes `/sys/class/spi_master/spi0`
  after `spi-dln2` probe; it does not create `/dev/spidev*` without a described
  SPI slave device. The helper uses modalias `dh2228fv`, one of the SPI IDs
  accepted by the mainline `spidev` driver.
- Built `tools/spi_test_device/spi_test_device.ko` successfully against the
  running kernel `7.0.11-arch1-1`.
- Built and flashed the updated C3 firmware to `/dev/ttyACM2`.
- Verified C3 command-plane responses:
  - `PING` returns `OK PONG c3-probe-v2 channels=8`.
  - `SPI START` returns `OK SPI START sclk=4 mosi=5 miso=6 cs=7 max=64`.
  - `SPI STATUS` reports active SPI with zero completed transfers.
  - `SPI STOP` returns `OK SPI STOP`.
- SPI transfer validation is blocked until root loads the temporary spidev
  helper, because the current Codex process cannot run interactive `sudo`.
- A first root-side load using generic modalias `spidev` created
  `/sys/bus/spi/devices/spi0.0` but did not bind the `spidev` driver or create
  `/dev/spidev0.0`; the helper default was changed to `dh2228fv` and needs a
  root reload before running the transfer test.
- Root reloaded the helper with modalias `dh2228fv`, and Linux created
  `/dev/spidev0.0` bound to the `spidev` driver.
- Verified one DLN-2 SPI full-duplex transfer at 100 kHz, 16 bytes:
  host RX matched the C3 MISO pattern `a0..af`, and the C3 recorded host MOSI
  bytes `10..1f`.
- Verified a SPI matrix at 10 kHz, 100 kHz, 500 kHz, and 1 MHz with 1, 4, 16,
  and 64 byte transfers. All 16 combinations passed full-duplex validation.
- A 256-byte transfer initially failed with Linux `EREMOTEIO`; C3 saw only a
  one-byte transaction. The issue is consistent with the S3 firmware still
  using ESP-IDF non-DMA SPI master mode, which is limited by the 64-byte FIFO.
  The S3 SPI master path was changed to `SPI_DMA_CH_AUTO` so the firmware can
  exercise the full Linux `spi-dln2` 256-byte transfer size.
- Built and flashed the S3 SPI DMA firmware to `/dev/ttyACM1`.
- Verified after S3 reflash:
  - `lsusb -d a257:2013` reports the DLN-2 compatible device.
  - `gpiodetect` reports `gpiochip2 [dln2] (10 lines)`.
  - `i2cdetect -l` reports the `dln2-i2c` adapter at `/dev/i2c-4`.
  - `/sys/class/spi_master/spi0` exists.
  - `/sys/bus/iio/devices/iio:device0/name` reports `dln2-adc`.
- The S3 USB re-enumeration removed the temporary `/dev/spidev0.0` child. A
  root reload of `tools/spi_test_device/spi_test_device.ko` is required before
  the 256-byte SPI retest can run.
- Attempting to unload the first-generation helper after S3 re-enumeration
  triggered a kernel Oops in `spi_test_device_exit()` because the module kept a
  stale `spi_device` pointer after the parent SPI controller was removed. The
  old module is stuck in the current kernel with refcount `-1`; avoid further
  `rmmod spi_test_device` attempts and reboot later to clear it.
- Added replacement helper `spi_dln2_spidev_test.ko`. It uses a different
  module name, holds an explicit `spi_device` reference, and checks
  `device_is_registered()` before unregistering the temporary child.
- Removed the first-generation `spi_test_device.c` helper source from the
  tracked tooling path to avoid accidental reuse.
- User loaded `spi_dln2_spidev_test.ko`, and Linux created `/dev/spidev0.0`
  bound to `spidev`.
- Verified after the S3/C3 DMA changes:
  - 64-byte full-duplex transfers pass at 100 kHz and 1 MHz.
  - 256-byte full-duplex transfers pass at 100 kHz and 1 MHz.
  - C3 records the expected MOSI stream and Linux host RX matches the C3 MISO
    pattern for every tested transfer.
  - `/sys/bus/spi/devices/spi0.0/statistics` reports 4 messages, 640 TX bytes,
    640 RX bytes, and `errors=0` for the final retest run.
- Extended the ESP32-C3 test firmware to version `c3-probe-v3`. `SPI START`
  now accepts an optional SPI mode argument and restarts the C3 slave if the
  requested mode changes.
- Extended `scripts/spi_c3_slave_test.py` with `--mode 0..3` and
  `--direction full|read|write` so the host can exercise Linux
  `DLN2_SPI_READ_WRITE`, `DLN2_SPI_READ`, and `DLN2_SPI_WRITE` paths.
- Updated the S3 SPI firmware so `DLN2_SPI_SET_FREQUENCY` and
  `DLN2_SPI_SET_MODE` reconfigure the active ESP-IDF SPI device instead of
  only updating firmware shadow state.
- Rebuilt and flashed the updated C3 firmware to `/dev/ttyACM2`, then rebuilt
  and flashed the updated S3 firmware to `/dev/ttyACM1`.
- User reloaded `spi_dln2_spidev_test.ko` after S3 USB re-enumeration, and
  `/dev/spidev0.0` was restored.
- Verified SPI full-duplex 16-byte transfers at 100 kHz in modes 0, 1, and 2.
- Verified SPI read-only and write-only transfers:
  - mode 0, 16-byte and 256-byte read-only transfers passed.
  - mode 0, 16-byte and 256-byte write-only transfers passed.
  - modes 1 and 2, 16-byte read-only transfers passed.
  - modes 1 and 2, 16-byte write-only transfers passed.
- Re-ran a short SPI regression after the final documentation update:
  modes 0/1/2 16-byte full-duplex plus mode 0 256-byte read-only/write-only
  all passed. Linux SPI statistics reported `messages=17` and `errors=0`.
- Hardened the S3 SPI reconfiguration failure path so a failed ESP-IDF
  re-add leaves the DLN-2 SPI port disabled rather than enabled with a null
  SPI device handle. This guard is build-verified in the S3 firmware and will
  be present after the next S3 flash.
- SPI mode 3 still fails against the current C3 fixture. The observed failure
  is a one-bit/byte alignment mismatch where the C3 reports a 17-byte transfer
  for a requested 16-byte transaction. Treat mode 3 as unresolved rather than
  validated.

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
