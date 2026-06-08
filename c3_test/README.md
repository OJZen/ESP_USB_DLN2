# ESP32-C3 Test Fixture

This firmware turns an ESP32-C3 board into a USB-Serial/JTAG controlled test
fixture for the ESP32-S3 DLN-2 firmware. It currently supports GPIO probing,
SPI slave testing, and I2C slave testing.

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

## Fixed No-Rewire Harness v1

This is the canonical lab wiring for repeatable regression tests. Wire it once
and keep it in place while switching between GPIO, I2C, and SPI tests in
software.

Connect only 3.3 V GPIO signals. Do not connect 5 V to any GPIO.
Also connect C3 GND to S3 GND. If both boards are powered by USB, do not
connect their 5 V rails together.

Push-pull GPIO/SPI signal lines should use a series resistor between C3 and S3.
Use 1 kOhm to 4.7 kOhm; 2.2 kOhm is a good default. I2C needs external 3.3 V
pullups on SDA and SCL; around 4.7 kOhm is a good default.

| Purpose | C3 signal / channel | C3 GPIO | Protection | S3 signal / Linux line | S3 GPIO |
| ------- | ------------------- | ------- | ---------- | ---------------------- | ------- |
| I2C SDA | SDA                 | GPIO0   | 3.3 V pullup | SDA                  | GPIO38  |
| I2C SCL | SCL                 | GPIO1   | 3.3 V pullup | SCL                  | GPIO47  |
| SPI SCLK | SCLK               | GPIO4   | series resistor | SCLK               | GPIO10  |
| SPI MOSI | MOSI               | GPIO5   | series resistor | MOSI               | GPIO11  |
| SPI MISO | MISO               | GPIO6   | series resistor | MISO               | GPIO12  |
| SPI CS0 | CS                  | GPIO7   | series resistor | CS0                 | GPIO13  |
| GPIO smoke A | channel 2      | GPIO3   | series resistor | DLN line 0          | GPIO1   |
| GPIO smoke B | channel 7      | GPIO10  | series resistor | DLN line 1          | GPIO2   |
| Ground | GND                  | GND     | direct | GND                      | GND     |

This fixed harness supports:

- I2C slave tests at address `0x52`.
- SPI slave tests for Linux S3 SPI master transfers.
- GPIO read/write and IRQ smoke tests on S3 DLN lines 0 and 1.

It intentionally does not wire C3 channels 0, 1, and 3-6 to S3 DLN GPIO lines,
because those C3 pins are reserved for the fixed I2C/SPI fixture. Do not add
parallel GPIO wires to those pins while using this no-rewire harness.

Use this command for the fixed GPIO smoke/IRQ test:

```bash
./scripts/c3_closed_loop_test.py --c3-port /dev/ttyACM2 --lines "0 1" --channels "2 7"
```

Use this command for the default fixed-harness regression run:

```bash
./scripts/c3_fixed_harness_regression.sh
```

## Legacy GPIO-Only Wiring

Use this wiring only when you want broad GPIO-only coverage and the fixed
I2C/SPI fixture wires are not connected. All signal lines should use a series
resistor between C3 and S3. Use 1 kOhm to 4.7 kOhm; 2.2 kOhm is a good default.

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

This default wiring covers S3 DLN lines 0-7. To test the remaining current
S3 DLN lines 8-9 later, move two C3 probe channels to those S3 GPIOs and pass
those Linux line offsets to the host test script with `--lines`.

## GPIO Serial Commands

The firmware accepts newline-terminated commands on the ESP-IDF console port at
115200 baud. The project defaults this console to the C3 USB-Serial/JTAG port
with `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`.

- `PING`
- `MAP`
- `STATUS`
- `READ`
- `RESET`
- `IN <mask>`
- `DRIVE <mask> <value>`
- `PULSE <mask> <count> <high_ms> <low_ms>`
- `I2C START [addr]|STOP|STATUS|RX <len>|LOAD <len>`
- `SPI START [mode]|STOP|STATUS|XFER <len> [timeout_ms]`

Masks use C3 channel bits. For example, channel 0 is bit `0x01`, channel 7 is
bit `0x80`.

`RESET` returns all GPIO probe channels to input with pulldown enabled.
`PULSE` drives the selected channel mask for the requested pulse train and then
releases those channels back to input.

## I2C Fixture Wiring

The fixed no-rewire harness already includes these connections. Use 3.3 V
pullups on SDA and SCL. Around 4.7 kOhm is a good default. Do not pull the bus
up to 5 V.

| C3 signal | C3 GPIO | S3 signal | S3 GPIO |
| --- | --- | --- | --- |
| SDA | GPIO0 | SDA | GPIO38 |
| SCL | GPIO1 | SCL | GPIO47 |
| GND | GND | GND | GND |

The default slave address is `0x52`.

## SPI Fixture Wiring

The fixed no-rewire harness already includes these connections. The SPI fixture
uses the C3 as a SPI slave and the S3 as the DLN-2-backed Linux SPI master.

| C3 signal | C3 GPIO | S3 signal | S3 GPIO |
| --- | --- | --- | --- |
| SCLK | GPIO4 | SCLK | GPIO10 |
| MOSI | GPIO5 | MOSI | GPIO11 |
| MISO | GPIO6 | MISO | GPIO12 |
| CS | GPIO7 | CS0 | GPIO13 |
| GND | GND | GND | GND |

Use short wires. Series resistors on push-pull SPI lines are acceptable for lab
protection; 1 kOhm to 2.2 kOhm has worked in this setup.

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

The firmware prints `READY c3-probe-v4 channels=8` when it is running.

## Fixed-Harness Regression

After wiring the fixed no-rewire harness and installing host support on the
Linux host:

```bash
./scripts/c3_fixed_harness_regression.sh
```

The default run covers GPIO smoke/IRQ on lines 0 and 1, I2C basic transfers,
and one SPI full-duplex transfer. Add `--with-i2c-stress` or
`--with-spi-matrix` for broader checks.

## Closed-Loop GPIO Test

With the fixed no-rewire harness:

```bash
./scripts/c3_closed_loop_test.py --c3-port /dev/ttyACM2 --lines "0 1" --channels "2 7"
```

With the legacy GPIO-only eight-line wiring:

```bash
./scripts/c3_closed_loop_test.py --c3-port <C3_PORT>
```

With legacy GPIO-only wiring, if channel 7 / C3 GPIO10 is not wired, test only
the first seven channels:

```bash
./scripts/c3_closed_loop_test.py --c3-port <C3_PORT> --lines "0 1 2 3 4 5 6"
```

The script uses the existing S3 DLN-2 GPIO device at
`/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00` by default. It tests:

- S3 output through Linux `gpiod`, observed by C3 input.
- C3 output, observed by S3 input through Linux `gpiod`.
- C3 pulse generation on every selected channel, observed by Linux `gpiomon`
  events.

## SPI Fixture Test

After wiring SPI and installing host support on the Linux host:

```bash
./scripts/spi_c3_slave_test.py --c3 /dev/ttyACM2 --spidev /dev/spidev0.0 --mode 0 --speed 100000 --length 16 --direction full
```

The script can also test `--direction read` and `--direction write`, SPI modes
0-3, and transfer lengths up to 256 bytes.

## I2C Fixture Test

After wiring I2C with 3.3 V pullups:

```bash
./scripts/i2c_c3_slave_test.py --c3 /dev/ttyACM2 --addr 0x52 --lengths "16"
./scripts/i2c_c3_stress_test.py --c3 /dev/ttyACM2 --addr 0x52 --bad-addr 0x53 --cycles 50
```

The I2C test script auto-detects the DLN-2 I2C adapter by scanning
`/sys/class/i2c-dev/i2c-*/name` for `dln2-i2c`.
