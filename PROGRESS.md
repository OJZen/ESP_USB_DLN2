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

## Log

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
