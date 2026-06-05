#!/usr/bin/env python3
import argparse
import fcntl
import os
import select
import sys
import termios
import time


I2C_SLAVE = 0x0703
I2C_TEST_MAX_BYTES = 256
DEFAULT_I2C_ADDR = 0x52


def configure_serial(fd):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


class C3Console:
    def __init__(self, path, open_delay):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure_serial(self.fd)
        self.buf = b""
        time.sleep(open_delay)
        termios.tcflush(self.fd, termios.TCIFLUSH)

    def close(self):
        os.close(self.fd)

    def read_line(self, timeout=2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                return line.decode(errors="replace").strip()
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                chunk = os.read(self.fd, 4096)
                if chunk:
                    self.buf += chunk.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        return None

    def drain(self, timeout=0.2):
        end = time.monotonic() + timeout
        lines = []
        while time.monotonic() < end:
            line = self.read_line(timeout=0.05)
            if line is None:
                continue
            if line:
                lines.append(line)
        return lines

    def write(self, line):
        os.write(self.fd, (line + "\n").encode())

    def command(self, line, timeout=2.0):
        self.write(line)
        seen = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = self.read_line(timeout=0.1)
            if got is None:
                continue
            seen.append(got)
            if got.startswith("OK ") or got.startswith("ERR "):
                return got
        raise TimeoutError(f"no response for {line!r}; seen={seen!r}")

    def sync(self, attempts):
        last_error = None
        for _ in range(attempts):
            try:
                return self.command("PING", timeout=1.5)
            except TimeoutError as exc:
                last_error = exc
                self.buf = b""
                termios.tcflush(self.fd, termios.TCIFLUSH)
                time.sleep(0.3)
        raise TimeoutError(f"C3 did not answer PING after {attempts} attempts; last={last_error}")


def find_dln2_i2c_dev():
    base = "/sys/class/i2c-dev"
    for name in sorted(os.listdir(base)):
        name_path = os.path.join(base, name, "name")
        try:
            with open(name_path, "r", encoding="utf-8") as f:
                adapter_name = f.read().strip()
        except OSError:
            continue
        if "dln2-i2c" in adapter_name:
            return f"/dev/{name}"
    raise FileNotFoundError("no dln2-i2c adapter found under /sys/class/i2c-dev")


def i2c_open(path, addr):
    fd = os.open(path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, I2C_SLAVE, addr)
    except Exception:
        os.close(fd)
        raise
    return fd


def i2c_write(path, addr, data):
    fd = i2c_open(path, addr)
    try:
        written = os.write(fd, data)
        if written != len(data):
            raise OSError(f"short I2C write: {written}/{len(data)}")
    finally:
        os.close(fd)


def i2c_read(path, addr, length):
    fd = i2c_open(path, addr)
    try:
        data = os.read(fd, length)
        if len(data) != length:
            raise OSError(f"short I2C read: {len(data)}/{length}")
        return data
    finally:
        os.close(fd)


def parse_status(line):
    parts = {}
    for item in line.split():
        if "=" in item:
            key, value = item.split("=", 1)
            parts[key] = value
    if "rx" not in parts or "tx" not in parts:
        raise ValueError(f"bad C3 I2C status line: {line}")
    return parts


def wait_for_rx(c3, expected_len, timeout):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        line = c3.command("I2C STATUS", timeout=1.0)
        last = line
        parts = parse_status(line)
        if int(parts.get("rx_done", "0"), 0) and int(parts.get("rx_len", "0"), 0) == expected_len:
            return parts, line
        time.sleep(0.05)
    raise TimeoutError(f"C3 did not report I2C RX completion; last={last!r}")


def parse_lengths(text):
    lengths = [int(item, 0) for item in text.split()]
    for length in lengths:
        if length < 1 or length > I2C_TEST_MAX_BYTES:
            raise SystemExit("--lengths entries must be 1..256")
    return lengths


def main():
    parser = argparse.ArgumentParser(description="Validate S3 DLN2 I2C against C3 I2C slave firmware.")
    parser.add_argument("--c3", default="/dev/ttyACM2", help="C3 USB serial device")
    parser.add_argument("--i2c-dev", default=None, help="Linux i2c-dev node; defaults to first dln2-i2c adapter")
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=DEFAULT_I2C_ADDR, help="7-bit C3 I2C slave address")
    parser.add_argument("--lengths", default="1 4 16 64 256", help="space-separated transfer lengths")
    parser.add_argument("--timeout", type=float, default=3.0, help="timeout in seconds for C3 RX completion")
    parser.add_argument("--open-delay", type=float, default=1.0, help="delay after opening the C3 serial port")
    parser.add_argument("--sync-attempts", type=int, default=5, help="number of PING attempts used to synchronize with C3")
    args = parser.parse_args()

    if args.addr < 1 or args.addr > 0x7f:
        raise SystemExit("--addr must be a 7-bit nonzero I2C address")

    i2c_dev = args.i2c_dev or find_dln2_i2c_dev()
    lengths = parse_lengths(args.lengths)
    print(f"i2c_dev={i2c_dev} c3={args.c3} addr=0x{args.addr:02x} lengths={lengths}")

    c3 = C3Console(args.c3, args.open_delay)
    try:
        c3.drain()
        print(c3.sync(args.sync_attempts))
        print(c3.command(f"I2C START 0x{args.addr:02x}"))

        ok = True
        for length in lengths:
            tx_data = bytes((0x10 + i) & 0xff for i in range(length))
            print(c3.command(f"I2C RX {length}"))
            i2c_write(i2c_dev, args.addr, tx_data)
            parts, line = wait_for_rx(c3, length, args.timeout)
            c3_rx = bytes.fromhex(parts["rx"])
            print(line)
            if c3_rx != tx_data:
                print(f"FAIL write len={length}: c3_rx expected {tx_data.hex()}", file=sys.stderr)
                ok = False
            else:
                print(f"PASS write len={length}")

            expected_read = bytes(0xc0 + (i & 0x3f) for i in range(length))
            print(c3.command(f"I2C LOAD {length}"))
            read_data = i2c_read(i2c_dev, args.addr, length)
            print(f"host_read len={length} data={read_data.hex()}")
            if read_data != expected_read:
                print(f"FAIL read len={length}: expected {expected_read.hex()}", file=sys.stderr)
                ok = False
            else:
                print(f"PASS read len={length}")

        print("PASS I2C transfer matrix" if ok else "FAIL I2C transfer matrix")
        return 0 if ok else 1
    finally:
        try:
            print(c3.command("I2C STOP", timeout=1.0))
        except Exception:
            pass
        c3.close()


if __name__ == "__main__":
    raise SystemExit(main())
