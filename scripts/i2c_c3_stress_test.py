#!/usr/bin/env python3
import argparse
import errno
import os
import sys
import time

from i2c_c3_slave_test import (
    C3Console,
    c3_default_read_pattern,
    find_dln2_i2c_dev,
    i2c_rdwr_write_read,
    i2c_read,
    i2c_write,
    smbus_read_byte_data,
    smbus_write_byte_data,
    wait_for_rx,
)


def parse_int_list(text, low, high, name):
    values = [int(item, 0) for item in text.split()]
    for value in values:
        if value < low or value > high:
            raise SystemExit(f"{name} entries must be {low}..{high}")
    return values


def assert_equal(label, got, expected):
    if got != expected:
        raise AssertionError(
            f"{label}: got {got.hex()}, expected {expected.hex()}"
        )


def expect_os_error(label, fn):
    try:
        fn()
    except OSError as exc:
        err = exc.errno if exc.errno is not None else errno.EIO
        print(f"PASS {label}: failed as expected errno={err} {os.strerror(err)}")
        return
    raise AssertionError(f"{label}: unexpectedly succeeded")


def c3_rx_bytes(c3, expected_len, timeout):
    parts, _ = wait_for_rx(c3, expected_len, timeout)
    return bytes.fromhex(parts["rx"])


def verify_raw_write(c3, i2c_dev, addr, length, seed, timeout):
    tx_data = bytes((seed + i) & 0xff for i in range(length))
    c3.command(f"I2C RX {length}")
    i2c_write(i2c_dev, addr, tx_data)
    assert_equal("raw write", c3_rx_bytes(c3, length, timeout), tx_data)


def verify_raw_read(c3, i2c_dev, addr, length):
    expected = c3_default_read_pattern(length)
    c3.command(f"I2C LOAD {length}")
    assert_equal("raw read", i2c_read(i2c_dev, addr, length), expected)


def verify_rdwr(c3, i2c_dev, addr, register, length, timeout):
    expected = c3_default_read_pattern(length)
    c3.command("I2C RX 1")
    c3.command(f"I2C LOAD {length}")
    got = i2c_rdwr_write_read(i2c_dev, addr, bytes([register]), length)
    assert_equal("rdwr register byte", c3_rx_bytes(c3, 1, timeout), bytes([register]))
    assert_equal("rdwr read", got, expected)


def verify_smbus(c3, i2c_dev, addr, command, timeout):
    value = (command ^ 0xa5) & 0xff
    c3.command("I2C RX 2")
    smbus_write_byte_data(i2c_dev, addr, command, value)
    assert_equal(
        "smbus write_byte_data",
        c3_rx_bytes(c3, 2, timeout),
        bytes([command, value]),
    )

    c3.command("I2C RX 1")
    c3.command("I2C LOAD 1")
    got = smbus_read_byte_data(i2c_dev, addr, command)
    assert_equal("smbus read command", c3_rx_bytes(c3, 1, timeout), bytes([command]))
    expected = c3_default_read_pattern(1)[0]
    if got != expected:
        raise AssertionError(f"smbus read_byte_data: got 0x{got:02x}, expected 0x{expected:02x}")


def verify_good_transaction(c3, i2c_dev, addr, timeout):
    verify_raw_write(c3, i2c_dev, addr, 4, 0x40, timeout)
    verify_raw_read(c3, i2c_dev, addr, 4)


def run_error_recovery(c3, i2c_dev, addr, bad_addr, timeout):
    print(f"TEST error recovery bad_addr=0x{bad_addr:02x}")

    expect_os_error(
        "bad-address raw write",
        lambda: i2c_write(i2c_dev, bad_addr, b"\x00"),
    )
    verify_good_transaction(c3, i2c_dev, addr, timeout)

    expect_os_error(
        "bad-address raw read",
        lambda: i2c_read(i2c_dev, bad_addr, 1),
    )
    verify_good_transaction(c3, i2c_dev, addr, timeout)

    expect_os_error(
        "bad-address I2C_RDWR",
        lambda: i2c_rdwr_write_read(i2c_dev, bad_addr, b"\x10", 1),
    )
    verify_good_transaction(c3, i2c_dev, addr, timeout)

    print("PASS error recovery")


def run_stress(c3, i2c_dev, addr, cycles, lengths, registers, timeout):
    print(f"TEST mixed stress cycles={cycles} lengths={lengths} registers={registers}")
    start = time.monotonic()
    for cycle in range(cycles):
        length = lengths[cycle % len(lengths)]
        register = registers[cycle % len(registers)]

        verify_raw_write(c3, i2c_dev, addr, length, 0x10 + cycle, timeout)
        verify_raw_read(c3, i2c_dev, addr, length)
        verify_rdwr(c3, i2c_dev, addr, register, length, timeout)
        verify_smbus(c3, i2c_dev, addr, register, timeout)

        if (cycle + 1) % 10 == 0 or cycle + 1 == cycles:
            elapsed = time.monotonic() - start
            print(f"PASS cycle {cycle + 1}/{cycles} elapsed={elapsed:.2f}s")

    print("PASS mixed stress")


def main():
    parser = argparse.ArgumentParser(
        description="Run DLN2 I2C error-recovery and mixed stress tests against the C3 fixture."
    )
    parser.add_argument("--c3", default="/dev/ttyACM2", help="C3 USB serial device")
    parser.add_argument("--i2c-dev", default=None, help="Linux i2c-dev node; defaults to first dln2-i2c adapter")
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=0x52, help="7-bit C3 I2C slave address")
    parser.add_argument("--bad-addr", type=lambda x: int(x, 0), default=0x53, help="unused 7-bit address for NACK tests")
    parser.add_argument("--cycles", type=int, default=50, help="mixed stress cycles")
    parser.add_argument("--lengths", default="1 4 16 64 256", help="space-separated transfer lengths, 1..256")
    parser.add_argument("--registers", default="0x00 0x10 0x7f", help="space-separated 8-bit register values")
    parser.add_argument("--timeout", type=float, default=3.0, help="timeout for C3 RX completion")
    parser.add_argument("--open-delay", type=float, default=1.0, help="delay after opening the C3 serial port")
    parser.add_argument("--sync-attempts", type=int, default=5, help="number of PING attempts used to synchronize with C3")
    parser.add_argument("--skip-errors", action="store_true", help="skip NACK/error recovery tests")
    parser.add_argument("--skip-stress", action="store_true", help="skip mixed stress tests")
    args = parser.parse_args()

    if args.addr < 1 or args.addr > 0x7f:
        raise SystemExit("--addr must be a 7-bit nonzero I2C address")
    if args.bad_addr < 1 or args.bad_addr > 0x7f:
        raise SystemExit("--bad-addr must be a 7-bit nonzero I2C address")
    if args.bad_addr == args.addr:
        raise SystemExit("--bad-addr must differ from --addr")
    if args.cycles < 1:
        raise SystemExit("--cycles must be positive")
    if args.skip_errors and args.skip_stress:
        raise SystemExit("nothing to test: both --skip-errors and --skip-stress are set")

    i2c_dev = args.i2c_dev or find_dln2_i2c_dev()
    lengths = parse_int_list(args.lengths, 1, 256, "--lengths")
    registers = parse_int_list(args.registers, 0, 255, "--registers")
    print(
        f"i2c_dev={i2c_dev} c3={args.c3} addr=0x{args.addr:02x} "
        f"bad_addr=0x{args.bad_addr:02x} cycles={args.cycles}"
    )

    c3 = C3Console(args.c3, args.open_delay)
    try:
        c3.drain()
        print(c3.sync(args.sync_attempts))
        print(c3.command(f"I2C START 0x{args.addr:02x}"))

        if not args.skip_errors:
            run_error_recovery(c3, i2c_dev, args.addr, args.bad_addr, args.timeout)
        if not args.skip_stress:
            run_stress(c3, i2c_dev, args.addr, args.cycles, lengths, registers, args.timeout)

        print("PASS I2C stability test")
        return 0
    finally:
        try:
            print(c3.command("I2C STOP", timeout=1.0))
        except Exception:
            pass
        c3.close()


if __name__ == "__main__":
    raise SystemExit(main())
