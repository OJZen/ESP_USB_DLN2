"""Live Windows smoke test; requires a bare S3 with no external wires/devices."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from dln2_windows import DLN2, DeviceError, GPIO_PINS, WinUSBTransport


def run(device):
    report = {"info": device.info(), "gpio": [], "adc_raw": {}}
    for pin, gpio in enumerate(GPIO_PINS):
        input_value = device.gpio_read(pin)
        try:
            for value in (0, 1):
                device.gpio_write(pin, value)
                data = struct.pack("<H", pin)
                assert struct.unpack("<HB", device.request(2, 0x010D, data)) == (pin, value)
                assert struct.unpack("<HB", device.request(2, 0x0114, data)) == (pin, 1)
        finally:
            device.gpio_release(pin)
        report["gpio"].append({"line": pin, "gpio": gpio, "input": input_value,
                               "output_shadow_and_direction": "PASS"})

    for size in [0] * 20 + [256]:
        try:
            device.i2c_write(0x52, bytes(size))
        except DeviceError as error:
            assert error.command == 0x0306 and error.result == 0xBA
        else:
            raise AssertionError("Expected NACK on bare-board I2C address 0x52")
    for size in (1, 256):
        try:
            device.i2c_read(0x52, size)
        except DeviceError as error:
            assert error.command == 0x0307 and error.result == 0xBA
        else:
            raise AssertionError("Expected NACK on bare-board I2C address 0x52")
    report["i2c_nack_recovery"] = {"attempts": 23, "result": "PASS"}

    transfers = 0
    for mode in range(4):
        for size in (1, 52, 53, 64, 128, 256):
            assert len(device.spi_transfer(bytes(range(size)), 100000, mode)) == size
            transfers += 1
    for speed in (10000, 10000000):
        assert len(device.spi_transfer(b"\x9f\0\0\0", speed)) == 4
        transfers += 1
    report["spi"] = {"transfers": transfers, "modes": [0, 1, 2, 3],
                     "lengths": [1, 52, 53, 64, 128, 256], "result": "PASS"}

    for channel in range(4):
        values = [device.adc_read(channel) for _ in range(5)]
        assert all(0 <= value <= 1023 for value in values)
        report["adc_raw"][channel] = values

    device.request(5, 0x0602, b"\0")
    try:
        device.request(5, 0x0605, b"\0\0")
        try:
            device.request(5, 0x060C, struct.pack("<BBBHHH", 0, 0, 5, 10, 0, 1023))
            try:
                for _ in range(20):
                    assert device.next_event(timeout=1) == (0x0610, 0, b"")
                    value, = struct.unpack("<H", device.request(5, 0x060A, b"\0\0"))
                    assert 0 <= value <= 1023
            finally:
                device.request(5, 0x060C, bytes(9))
        finally:
            device.request(5, 0x0606, b"\0\0")
    finally:
        device.request(5, 0x0603, b"\0")
    report["adc_events_interleaved_with_responses"] = 20
    assert device.info() == report["info"]
    report["result"] = "PASS"
    report["limits"] = ("GPIO checks latch/direction only; no measured output voltage. "
                        "I2C NACK recovery and SPI transfer lengths do not prove slave data. "
                        "Floating ADC inputs do not prove calibration. Linux USB binding untested here.")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bare-board", action="store_true",
                        help="confirm no external wires/devices; test drives all exposed GPIO outputs")
    args = parser.parse_args()
    if not args.bare_board:
        parser.error("--bare-board is required; disconnect all external wiring first")
    with DLN2(WinUSBTransport()) as device:
        print(json.dumps(run(device), indent=2))
