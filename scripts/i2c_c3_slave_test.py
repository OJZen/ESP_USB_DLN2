#!/usr/bin/env python3
import argparse
import ctypes
import fcntl
import os
import select
import sys
import termios
import time


I2C_SLAVE = 0x0703
I2C_RDWR = 0x0707
I2C_SMBUS = 0x0720
I2C_M_RD = 0x0001
I2C_SMBUS_READ = 1
I2C_SMBUS_WRITE = 0
I2C_SMBUS_BYTE = 1
I2C_SMBUS_BYTE_DATA = 2
I2C_SMBUS_WORD_DATA = 3
I2C_SMBUS_I2C_BLOCK_DATA = 8
I2C_SMBUS_BLOCK_MAX = 32
I2C_TEST_MAX_BYTES = 256
DEFAULT_I2C_ADDR = 0x52


class I2CSmbusData(ctypes.Union):
    _fields_ = [
        ("byte", ctypes.c_uint8),
        ("word", ctypes.c_uint16),
        ("block", ctypes.c_uint8 * (I2C_SMBUS_BLOCK_MAX + 2)),
    ]


class I2CSmbusIoctlData(ctypes.Structure):
    _fields_ = [
        ("read_write", ctypes.c_uint8),
        ("command", ctypes.c_uint8),
        ("size", ctypes.c_uint32),
        ("data", ctypes.POINTER(I2CSmbusData)),
    ]


class I2CMsg(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("len", ctypes.c_uint16),
        ("buf", ctypes.POINTER(ctypes.c_uint8)),
    ]


class I2CRdwrIoctlData(ctypes.Structure):
    _fields_ = [
        ("msgs", ctypes.POINTER(I2CMsg)),
        ("nmsgs", ctypes.c_uint32),
    ]


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


def i2c_rdwr_write_read(path, addr, write_data, read_len):
    write_buf = (ctypes.c_uint8 * len(write_data))(*write_data)
    read_buf = (ctypes.c_uint8 * read_len)()
    msgs = (I2CMsg * 2)(
        I2CMsg(
            addr=addr,
            flags=0,
            len=len(write_data),
            buf=ctypes.cast(write_buf, ctypes.POINTER(ctypes.c_uint8)),
        ),
        I2CMsg(
            addr=addr,
            flags=I2C_M_RD,
            len=read_len,
            buf=ctypes.cast(read_buf, ctypes.POINTER(ctypes.c_uint8)),
        ),
    )
    ioctl_data = I2CRdwrIoctlData(
        msgs=ctypes.cast(msgs, ctypes.POINTER(I2CMsg)),
        nmsgs=2,
    )

    fd = os.open(path, os.O_RDWR)
    try:
        ret = fcntl.ioctl(fd, I2C_RDWR, ioctl_data)
        if ret != 2:
            raise OSError(f"I2C_RDWR returned {ret}, expected 2")
        return bytes(read_buf)
    finally:
        os.close(fd)


def i2c_smbus_access(path, addr, read_write, command, size, data):
    ioctl_data = I2CSmbusIoctlData(
        read_write=read_write,
        command=command,
        size=size,
        data=ctypes.pointer(data),
    )

    fd = i2c_open(path, addr)
    try:
        ret = fcntl.ioctl(fd, I2C_SMBUS, ioctl_data)
        if ret != 0:
            raise OSError(f"I2C_SMBUS returned {ret}, expected 0")
    finally:
        os.close(fd)


def smbus_receive_byte(path, addr):
    data = I2CSmbusData()
    i2c_smbus_access(path, addr, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, data)
    return data.byte


def smbus_send_byte(path, addr, value):
    data = I2CSmbusData()
    i2c_smbus_access(path, addr, I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, data)


def smbus_read_byte_data(path, addr, command):
    data = I2CSmbusData()
    i2c_smbus_access(path, addr, I2C_SMBUS_READ, command,
                     I2C_SMBUS_BYTE_DATA, data)
    return data.byte


def smbus_write_byte_data(path, addr, command, value):
    data = I2CSmbusData()
    data.byte = value
    i2c_smbus_access(path, addr, I2C_SMBUS_WRITE, command,
                     I2C_SMBUS_BYTE_DATA, data)


def smbus_read_word_data(path, addr, command):
    data = I2CSmbusData()
    i2c_smbus_access(path, addr, I2C_SMBUS_READ, command,
                     I2C_SMBUS_WORD_DATA, data)
    return data.word


def smbus_write_word_data(path, addr, command, value):
    data = I2CSmbusData()
    data.word = value
    i2c_smbus_access(path, addr, I2C_SMBUS_WRITE, command,
                     I2C_SMBUS_WORD_DATA, data)


def smbus_read_i2c_block_data(path, addr, command, length):
    data = I2CSmbusData()
    data.block[0] = length
    i2c_smbus_access(path, addr, I2C_SMBUS_READ, command,
                     I2C_SMBUS_I2C_BLOCK_DATA, data)
    return bytes(data.block[1:1 + data.block[0]])


def smbus_write_i2c_block_data(path, addr, command, data_bytes):
    if len(data_bytes) > I2C_SMBUS_BLOCK_MAX:
        raise ValueError("SMBus I2C block write length must be <= 32")

    data = I2CSmbusData()
    data.block[0] = len(data_bytes)
    for index, value in enumerate(data_bytes):
        data.block[index + 1] = value
    i2c_smbus_access(path, addr, I2C_SMBUS_WRITE, command,
                     I2C_SMBUS_I2C_BLOCK_DATA, data)


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


def parse_registers(text):
    registers = [int(item, 0) for item in text.split()]
    for register in registers:
        if register < 0 or register > 0xff:
            raise SystemExit("--registers entries must be 0..255")
    return registers


def parse_smbus_block_lengths(text):
    lengths = [int(item, 0) for item in text.split()]
    for length in lengths:
        if length < 1 or length > I2C_SMBUS_BLOCK_MAX:
            raise SystemExit("--smbus-block-lengths entries must be 1..32")
    return lengths


def c3_default_read_pattern(length):
    return bytes(0xc0 + (i & 0x3f) for i in range(length))


def c3_smbus_write_pattern(command, length):
    return bytes((command + 0x20 + i) & 0xff for i in range(length))


def c3_expect_rx(c3, expected, timeout, label):
    parts, line = wait_for_rx(c3, len(expected), timeout)
    c3_rx = bytes.fromhex(parts["rx"])
    print(line)
    if c3_rx != expected:
        print(f"FAIL {label}: c3_rx expected {expected.hex()}", file=sys.stderr)
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description="Validate S3 DLN2 I2C against C3 I2C slave firmware.")
    parser.add_argument("--c3", default="/dev/ttyACM2", help="C3 USB serial device")
    parser.add_argument("--i2c-dev", default=None, help="Linux i2c-dev node; defaults to first dln2-i2c adapter")
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=DEFAULT_I2C_ADDR, help="7-bit C3 I2C slave address")
    parser.add_argument("--lengths", default="1 4 16 64 256", help="space-separated transfer lengths")
    parser.add_argument("--rdwr-lengths", default=None, help="space-separated I2C_RDWR register-read lengths; defaults to --lengths")
    parser.add_argument("--registers", default="0x00 0x10 0x7f", help="space-separated 8-bit register addresses for I2C_RDWR tests")
    parser.add_argument("--smbus-commands", default="0x00 0x10 0x7f", help="space-separated 8-bit SMBus command/register values")
    parser.add_argument("--smbus-block-lengths", default="1 4 16 32", help="space-separated SMBus I2C block lengths, 1..32")
    parser.add_argument("--timeout", type=float, default=3.0, help="timeout in seconds for C3 RX completion")
    parser.add_argument("--open-delay", type=float, default=1.0, help="delay after opening the C3 serial port")
    parser.add_argument("--sync-attempts", type=int, default=5, help="number of PING attempts used to synchronize with C3")
    parser.add_argument("--skip-raw", action="store_true", help="skip plain read/write transfer tests")
    parser.add_argument("--skip-rdwr", action="store_true", help="skip I2C_RDWR register-read tests")
    parser.add_argument("--skip-smbus", action="store_true", help="skip I2C_SMBUS ioctl tests")
    args = parser.parse_args()

    if args.addr < 1 or args.addr > 0x7f:
        raise SystemExit("--addr must be a 7-bit nonzero I2C address")

    i2c_dev = args.i2c_dev or find_dln2_i2c_dev()
    lengths = parse_lengths(args.lengths)
    rdwr_lengths = parse_lengths(args.rdwr_lengths or args.lengths)
    registers = parse_registers(args.registers)
    smbus_commands = parse_registers(args.smbus_commands)
    smbus_block_lengths = parse_smbus_block_lengths(args.smbus_block_lengths)
    print(
        f"i2c_dev={i2c_dev} c3={args.c3} addr=0x{args.addr:02x} "
        f"lengths={lengths} rdwr_lengths={rdwr_lengths} registers={registers} "
        f"smbus_commands={smbus_commands} smbus_block_lengths={smbus_block_lengths}"
    )

    c3 = C3Console(args.c3, args.open_delay)
    try:
        c3.drain()
        print(c3.sync(args.sync_attempts))
        print(c3.command(f"I2C START 0x{args.addr:02x}"))

        ok = True
        if args.skip_raw and args.skip_rdwr and args.skip_smbus:
            raise SystemExit("nothing to test: all test groups are skipped")

        for length in [] if args.skip_raw else lengths:
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

            expected_read = c3_default_read_pattern(length)
            print(c3.command(f"I2C LOAD {length}"))
            read_data = i2c_read(i2c_dev, args.addr, length)
            print(f"host_read len={length} data={read_data.hex()}")
            if read_data != expected_read:
                print(f"FAIL read len={length}: expected {expected_read.hex()}", file=sys.stderr)
                ok = False
            else:
                print(f"PASS read len={length}")

        for register in [] if args.skip_rdwr else registers:
            for length in rdwr_lengths:
                reg_data = bytes([register])
                expected_read = c3_default_read_pattern(length)
                print(c3.command("I2C RX 1"))
                print(c3.command(f"I2C LOAD {length}"))
                read_data = i2c_rdwr_write_read(i2c_dev, args.addr, reg_data, length)
                parts, line = wait_for_rx(c3, len(reg_data), args.timeout)
                c3_rx = bytes.fromhex(parts["rx"])
                print(line)
                print(f"host_rdwr reg=0x{register:02x} len={length} data={read_data.hex()}")

                if c3_rx != reg_data:
                    print(
                        f"FAIL rdwr reg=0x{register:02x} len={length}: "
                        f"c3_rx expected {reg_data.hex()}",
                        file=sys.stderr,
                    )
                    ok = False
                elif read_data != expected_read:
                    print(
                        f"FAIL rdwr reg=0x{register:02x} len={length}: "
                        f"expected {expected_read.hex()}",
                        file=sys.stderr,
                    )
                    ok = False
                else:
                    print(f"PASS rdwr reg=0x{register:02x} len={length}")

        if not args.skip_smbus:
            receive_expected = c3_default_read_pattern(1)[0]
            print(c3.command("I2C LOAD 1"))
            got = smbus_receive_byte(i2c_dev, args.addr)
            print(f"host_smbus receive_byte data={got:02x}")
            if got != receive_expected:
                print(f"FAIL smbus receive_byte: expected {receive_expected:02x}",
                      file=sys.stderr)
                ok = False
            else:
                print("PASS smbus receive_byte")

            send_value = 0x33
            print(c3.command("I2C RX 1"))
            smbus_send_byte(i2c_dev, args.addr, send_value)
            if c3_expect_rx(c3, bytes([send_value]), args.timeout,
                            "smbus send_byte"):
                print("PASS smbus send_byte")
            else:
                ok = False

            for command in smbus_commands:
                print(c3.command("I2C RX 1"))
                print(c3.command("I2C LOAD 1"))
                got = smbus_read_byte_data(i2c_dev, args.addr, command)
                rx_ok = c3_expect_rx(c3, bytes([command]), args.timeout,
                                     f"smbus read_byte_data cmd=0x{command:02x}")
                expected = c3_default_read_pattern(1)[0]
                print(f"host_smbus read_byte_data cmd=0x{command:02x} data={got:02x}")
                if not rx_ok or got != expected:
                    if got != expected:
                        print(
                            f"FAIL smbus read_byte_data cmd=0x{command:02x}: "
                            f"expected {expected:02x}",
                            file=sys.stderr,
                        )
                    ok = False
                else:
                    print(f"PASS smbus read_byte_data cmd=0x{command:02x}")

                value = (command ^ 0x5a) & 0xff
                expected_rx = bytes([command, value])
                print(c3.command("I2C RX 2"))
                smbus_write_byte_data(i2c_dev, args.addr, command, value)
                if c3_expect_rx(c3, expected_rx, args.timeout,
                                f"smbus write_byte_data cmd=0x{command:02x}"):
                    print(f"PASS smbus write_byte_data cmd=0x{command:02x}")
                else:
                    ok = False

                print(c3.command("I2C RX 1"))
                print(c3.command("I2C LOAD 2"))
                got_word = smbus_read_word_data(i2c_dev, args.addr, command)
                rx_ok = c3_expect_rx(c3, bytes([command]), args.timeout,
                                     f"smbus read_word_data cmd=0x{command:02x}")
                expected_word = int.from_bytes(c3_default_read_pattern(2), "little")
                print(
                    f"host_smbus read_word_data cmd=0x{command:02x} "
                    f"data=0x{got_word:04x}"
                )
                if not rx_ok or got_word != expected_word:
                    if got_word != expected_word:
                        print(
                            f"FAIL smbus read_word_data cmd=0x{command:02x}: "
                            f"expected 0x{expected_word:04x}",
                            file=sys.stderr,
                        )
                    ok = False
                else:
                    print(f"PASS smbus read_word_data cmd=0x{command:02x}")

                word_value = (0x6000 | command) & 0xffff
                expected_rx = bytes([command]) + word_value.to_bytes(2, "little")
                print(c3.command("I2C RX 3"))
                smbus_write_word_data(i2c_dev, args.addr, command, word_value)
                if c3_expect_rx(c3, expected_rx, args.timeout,
                                f"smbus write_word_data cmd=0x{command:02x}"):
                    print(f"PASS smbus write_word_data cmd=0x{command:02x}")
                else:
                    ok = False

                for length in smbus_block_lengths:
                    print(c3.command("I2C RX 1"))
                    print(c3.command(f"I2C LOAD {length}"))
                    block = smbus_read_i2c_block_data(i2c_dev, args.addr,
                                                      command, length)
                    rx_ok = c3_expect_rx(c3, bytes([command]), args.timeout,
                                         "smbus read_i2c_block_data "
                                         f"cmd=0x{command:02x} len={length}")
                    expected_block = c3_default_read_pattern(length)
                    print(
                        f"host_smbus read_i2c_block_data cmd=0x{command:02x} "
                        f"len={length} data={block.hex()}"
                    )
                    if not rx_ok or block != expected_block:
                        if block != expected_block:
                            print(
                                "FAIL smbus read_i2c_block_data "
                                f"cmd=0x{command:02x} len={length}: "
                                f"expected {expected_block.hex()}",
                                file=sys.stderr,
                            )
                        ok = False
                    else:
                        print(
                            f"PASS smbus read_i2c_block_data "
                            f"cmd=0x{command:02x} len={length}"
                        )

                    write_block = c3_smbus_write_pattern(command, length)
                    expected_rx = bytes([command]) + write_block
                    print(c3.command(f"I2C RX {len(expected_rx)}"))
                    smbus_write_i2c_block_data(i2c_dev, args.addr, command,
                                               write_block)
                    if c3_expect_rx(c3, expected_rx, args.timeout,
                                    "smbus write_i2c_block_data "
                                    f"cmd=0x{command:02x} len={length}"):
                        print(
                            f"PASS smbus write_i2c_block_data "
                            f"cmd=0x{command:02x} len={length}"
                        )
                    else:
                        ok = False

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
