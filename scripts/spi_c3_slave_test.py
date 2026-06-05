#!/usr/bin/env python3
import argparse
import ctypes
import fcntl
import os
import select
import struct
import sys
import termios
import time


SPI_TEST_MAX_BYTES = 256
SPI_IOC_MAGIC = ord("k")
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04

IOC_NRBITS = 8
IOC_TYPEBITS = 8
IOC_SIZEBITS = 14
IOC_DIRBITS = 2
IOC_NRSHIFT = 0
IOC_TYPESHIFT = IOC_NRSHIFT + IOC_NRBITS
IOC_SIZESHIFT = IOC_TYPESHIFT + IOC_TYPEBITS
IOC_DIRSHIFT = IOC_SIZESHIFT + IOC_SIZEBITS
IOC_WRITE = 1
SPI_IOC_TRANSFER_SIZE = struct.calcsize("=QQIIHBBBBBB")


def ioc(direction, ioctl_type, nr, size):
    return ((direction << IOC_DIRSHIFT) |
            (ioctl_type << IOC_TYPESHIFT) |
            (nr << IOC_NRSHIFT) |
            (size << IOC_SIZESHIFT))


def spi_ioc_message(count):
    size = count * SPI_IOC_TRANSFER_SIZE
    if size >= (1 << IOC_SIZEBITS):
        size = 0
    return ioc(IOC_WRITE, SPI_IOC_MAGIC, 0, size)


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
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure_serial(self.fd)
        self.buf = b""

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
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = self.read_line(timeout=0.1)
            if got is None:
                continue
            if got.startswith("OK ") or got.startswith("ERR "):
                return got
        raise TimeoutError(f"no response for {line!r}")


def spidev_transfer(path, tx_data, speed_hz):
    fd = os.open(path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, SPI_IOC_WR_MODE, struct.pack("B", 0))
        fcntl.ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, struct.pack("B", 8))
        fcntl.ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, struct.pack("I", speed_hz))

        tx = ctypes.create_string_buffer(bytes(tx_data))
        rx = ctypes.create_string_buffer(len(tx_data))
        transfer = struct.pack(
            "=QQIIHBBBBBB",
            ctypes.addressof(tx),
            ctypes.addressof(rx),
            len(tx_data),
            speed_hz,
            0,
            8,
            0,
            0,
            0,
            0,
            0,
        )
        fcntl.ioctl(fd, spi_ioc_message(1), transfer)
        return bytes(rx.raw)
    finally:
        os.close(fd)


def parse_spi_xfer(line):
    parts = {}
    for item in line.split():
        if "=" in item:
            key, value = item.split("=", 1)
            parts[key] = value
    if "rx" not in parts or "tx" not in parts:
        raise ValueError(f"bad C3 SPI line: {line}")
    return bytes.fromhex(parts["rx"]), bytes.fromhex(parts["tx"])


def main():
    parser = argparse.ArgumentParser(description="Validate S3 DLN2 SPI against C3 SPI slave firmware.")
    parser.add_argument("--c3", default="/dev/ttyACM2", help="C3 USB serial device")
    parser.add_argument("--spidev", default="/dev/spidev0.0", help="Linux spidev node")
    parser.add_argument("--length", type=int, default=16, help="transfer length, 1..256")
    parser.add_argument("--speed", type=int, default=100000, help="SPI speed in Hz")
    parser.add_argument("--timeout-ms", type=int, default=5000, help="C3 SPI arm timeout")
    args = parser.parse_args()

    if args.length < 1 or args.length > SPI_TEST_MAX_BYTES:
        raise SystemExit("--length must be 1..256")
    if not os.path.exists(args.spidev):
        raise SystemExit(
            f"{args.spidev} does not exist. Build tools/spi_test_device and load it with sudo first."
        )

    tx_data = bytes((0x10 + i) & 0xff for i in range(args.length))
    expected_miso = bytes(0xa0 + (i & 0x3f) for i in range(args.length))

    c3 = C3Console(args.c3)
    try:
        c3.drain()
        print(c3.command("PING"))
        print(c3.command("SPI START"))

        c3.write(f"SPI XFER {args.length} {args.timeout_ms}")
        time.sleep(0.1)
        rx_data = spidev_transfer(args.spidev, tx_data, args.speed)

        line = None
        deadline = time.monotonic() + (args.timeout_ms / 1000.0) + 1.0
        while time.monotonic() < deadline:
            got = c3.read_line(timeout=0.1)
            if got is None:
                continue
            if got.startswith("OK SPI XFER") or got.startswith("ERR SPI XFER"):
                line = got
                break
        if line is None:
            raise TimeoutError("C3 did not report SPI XFER result")
        print(line)
        if line.startswith("ERR "):
            raise RuntimeError(line)

        c3_rx, c3_tx = parse_spi_xfer(line)
        print(f"host_tx={tx_data.hex()} host_rx={rx_data.hex()}")

        ok = True
        if rx_data != expected_miso:
            print(f"FAIL host_rx expected {expected_miso.hex()}", file=sys.stderr)
            ok = False
        if c3_rx != tx_data:
            print(f"FAIL c3_rx expected {tx_data.hex()}", file=sys.stderr)
            ok = False
        if c3_tx != expected_miso:
            print(f"FAIL c3_tx expected {expected_miso.hex()}", file=sys.stderr)
            ok = False

        print("PASS SPI full-duplex transfer" if ok else "FAIL SPI full-duplex transfer")
        return 0 if ok else 1
    finally:
        try:
            print(c3.command("SPI STOP", timeout=1.0))
        except Exception:
            pass
        c3.close()


if __name__ == "__main__":
    raise SystemExit(main())
