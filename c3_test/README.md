# ESP32-C3 GPIO Test Probe

This firmware turns an ESP32-C3 board into a small serial-controlled GPIO
probe for testing the ESP32-S3 DLN-2 GPIO firmware.

## Default Channels

| C3 channel | ESP32-C3 GPIO |
| ---------- | ------------- |
| 0          | GPIO0         |
| 1          | GPIO1         |
| 2          | GPIO3         |
| 3          | GPIO4         |
| 4          | GPIO5         |
| 5          | GPIO6         |
| 6          | GPIO7         |
| 7          | GPIO10        |

The selected C3 pins intentionally avoid the common ESP32-C3 strapping pins
GPIO2, GPIO8, GPIO9, native USB pins GPIO18/GPIO19, UART0 pins GPIO20/GPIO21,
and flash pins.

## Wiring

Connect only 3.3 V GPIO signals. Do not connect 5 V to any GPIO.

All signal lines should use a series resistor between C3 and S3. Use 1 kOhm to
4.7 kOhm; 2.2 kOhm is a good default.

| C3 probe channel | C3 GPIO | Series resistor | S3 Linux line | S3 GPIO |
| ---------------- | ------- | --------------- | ------------- | ------- |
| 0                | GPIO0   | 1 kOhm-4.7 kOhm | 0             | GPIO1   |
| 1                | GPIO1   | 1 kOhm-4.7 kOhm | 1             | GPIO2   |
| 2                | GPIO3   | 1 kOhm-4.7 kOhm | 2             | GPIO4   |
| 3                | GPIO4   | 1 kOhm-4.7 kOhm | 3             | GPIO5   |
| 4                | GPIO5   | 1 kOhm-4.7 kOhm | 4             | GPIO6   |
| 5                | GPIO6   | 1 kOhm-4.7 kOhm | 5             | GPIO7   |
| 6                | GPIO7   | 1 kOhm-4.7 kOhm | 6             | GPIO8   |
| 7                | GPIO10  | 1 kOhm-4.7 kOhm | 7             | GPIO9   |

Also connect C3 GND to S3 GND. If both boards are powered by USB, do not
connect their 5 V rails together.

This default wiring covers S3 DLN lines 0-7. To test S3 lines 8-17 later, move
the same eight C3 probe channels to another batch of S3 GPIOs and pass those
Linux line offsets to the host test script with `--lines`.

## Serial Commands

The firmware accepts newline-terminated commands on the ESP-IDF console port at
115200 baud:

- `PING`
- `MAP`
- `STATUS`
- `READ`
- `RESET`
- `IN <mask>`
- `DRIVE <mask> <value>`
- `PULSE <mask> <count> <high_ms> <low_ms>`

Masks use C3 channel bits. For example, channel 0 is bit `0x01`, channel 7 is
bit `0x80`.

`RESET` returns all channels to input with pulldown enabled. `PULSE` drives the
selected channel mask for the requested pulse train and then releases those
channels back to input.

## Build

```bash
. /home/jzen/.espressif/v5.5.4/esp-idf/export.sh
cd c3_test
idf.py set-target esp32c3
idf.py build
```

## Flash

Use the C3 board's own serial port. The S3 board has been using
`/dev/ttyACM1`, so do not flash the C3 firmware to that port.

```bash
. /home/jzen/.espressif/v5.5.4/esp-idf/export.sh
idf.py -C c3_test -p <C3_PORT> flash monitor
```

The firmware prints `READY c3-gpio-probe-v1 channels=8` when it is running.

## Closed-Loop Test

After flashing the C3 and wiring the eight signal lines through series
resistors:

```bash
./scripts/c3_closed_loop_test.py --c3-port <C3_PORT>
```

The script uses the existing S3 DLN-2 GPIO device at
`/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00` by default. It tests:

- S3 output through Linux `gpiod`, observed by C3 input.
- C3 output, observed by S3 input through Linux `gpiod`.
- C3 pulse generation, observed by Linux `gpiomon` events.
