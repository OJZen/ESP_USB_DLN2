#!/usr/bin/env python3
import argparse
import ctypes
import errno
import fcntl
import os
import signal
import struct
import sys
import time
from dataclasses import dataclass

try:
    import gpiod
    from gpiod.line import Direction, Value
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:
    raise SystemExit(
        "missing Python dependency; install the gpiod Python binding and Pillow"
    ) from exc


I2C_SLAVE = 0x0703
SSD1306_DEFAULT_ADDR = 0x3C
SSD1306_WIDTH = 128
SSD1306_HEIGHT = 64
SSD1306_PAGES = SSD1306_HEIGHT // 8

SPI_IOC_MAGIC = ord("k")
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04

IOC_NRBITS = 8
IOC_TYPEBITS = 8
IOC_SIZEBITS = 14
IOC_NRSHIFT = 0
IOC_TYPESHIFT = IOC_NRSHIFT + IOC_NRBITS
IOC_SIZESHIFT = IOC_TYPESHIFT + IOC_TYPEBITS
IOC_DIRSHIFT = IOC_SIZESHIFT + IOC_SIZEBITS
IOC_WRITE = 1
SPI_IOC_TRANSFER_SIZE = struct.calcsize("=QQIIHBBBBBB")

DEFAULT_GPIO_CHIP = "/dev/gpio/by-id/usb-Diolan_DLN-2_USB_GPIO_12345678-if00"


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


def parse_int_list(text):
    return [int(item, 0) for item in text.replace(",", " ").split()]


def format_bytes(value):
    if value >= 1024 * 1024:
        return f"{value / (1024 * 1024):.1f}M"
    if value >= 1024:
        return f"{value / 1024:.1f}K"
    return str(value)


def find_dln2_i2c_dev():
    base = "/sys/class/i2c-dev"
    if not os.path.isdir(base):
        raise FileNotFoundError("/sys/class/i2c-dev is missing")

    for name in sorted(os.listdir(base)):
        name_path = os.path.join(base, name, "name")
        try:
            with open(name_path, "r", encoding="utf-8") as f:
                adapter_name = f.read().strip()
        except OSError:
            continue
        if "dln2-i2c" in adapter_name:
            return f"/dev/{name}"
    raise FileNotFoundError("no dln2-i2c adapter found")


def find_dln2_spidev(cs):
    for master in sorted(os.listdir("/sys/class/spi_master")
                         if os.path.isdir("/sys/class/spi_master") else []):
        if not master.startswith("spi"):
            continue
        bus = master[3:]
        target = os.path.realpath(os.path.join("/sys/class/spi_master", master))
        uevent = os.path.join(target, "device", "uevent")
        try:
            with open(uevent, "r", encoding="utf-8") as f:
                if "DRIVER=dln2-spi" not in f.read():
                    continue
        except OSError:
            continue
        dev = f"/dev/spidev{bus}.{cs}"
        if os.path.exists(dev):
            return dev
    default = f"/dev/spidev0.{cs}"
    if os.path.exists(default):
        return default
    raise FileNotFoundError("no DLN2 spidev node found")


class SSD1306:
    def __init__(self, i2c_dev, addr):
        self.i2c_dev = i2c_dev
        self.addr = addr
        self.fd = os.open(i2c_dev, os.O_RDWR)
        try:
            fcntl.ioctl(self.fd, I2C_SLAVE, addr)
            self.init()
        except Exception:
            os.close(self.fd)
            raise

    def close(self):
        os.close(self.fd)

    def write(self, data):
        os.write(self.fd, data)

    def command(self, *commands):
        if not commands:
            return
        self.write(bytes([0x00]) + bytes(commands))

    def init(self):
        self.command(0xAE)
        self.command(0xD5, 0x80)
        self.command(0xA8, 0x3F)
        self.command(0xD3, 0x00)
        self.command(0x40)
        self.command(0x8D, 0x14)
        self.command(0x20, 0x00)
        self.command(0xA1)
        self.command(0xC8)
        self.command(0xDA, 0x12)
        self.command(0x81, 0xCF)
        self.command(0xD9, 0xF1)
        self.command(0xDB, 0x40)
        self.command(0xA4)
        self.command(0xA6)
        self.clear()
        self.command(0xAF)

    def clear(self):
        self.show(Image.new("1", (SSD1306_WIDTH, SSD1306_HEIGHT), 0))

    def show(self, image):
        if image.size != (SSD1306_WIDTH, SSD1306_HEIGHT):
            raise ValueError("image must be 128x64")

        pixels = image.convert("1").load()
        self.command(0x21, 0, SSD1306_WIDTH - 1)
        self.command(0x22, 0, SSD1306_PAGES - 1)

        for page in range(SSD1306_PAGES):
            chunk = bytearray([0x40])
            y_base = page * 8
            for x in range(SSD1306_WIDTH):
                value = 0
                for bit in range(8):
                    if pixels[x, y_base + bit]:
                        value |= 1 << bit
                chunk.append(value)
            self.write(bytes(chunk))


class SpiFlash:
    def __init__(self, spidev, speed_hz, mode):
        self.spidev = spidev
        self.speed_hz = speed_hz
        self.mode = mode
        self.read_bytes = 0
        self.write_bytes = 0

    def transfer(self, tx_data):
        fd = os.open(self.spidev, os.O_RDWR)
        try:
            fcntl.ioctl(fd, SPI_IOC_WR_MODE, struct.pack("B", self.mode))
            fcntl.ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, struct.pack("B", 8))
            fcntl.ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ,
                        struct.pack("I", self.speed_hz))

            tx = ctypes.create_string_buffer(bytes(tx_data))
            rx = ctypes.create_string_buffer(len(tx_data))
            transfer = struct.pack(
                "=QQIIHBBBBBB",
                ctypes.addressof(tx),
                ctypes.addressof(rx),
                len(tx_data),
                self.speed_hz,
                0,
                8,
                0,
                0,
                0,
                0,
                0,
            )
            fcntl.ioctl(fd, spi_ioc_message(1), transfer)
            self.read_bytes += len(tx_data)
            return bytes(rx.raw)
        finally:
            os.close(fd)

    def read_jedec_id(self):
        rx = self.transfer(b"\x9f\x00\x00\x00")
        return rx[1:4]

    def read_status(self):
        rx = self.transfer(b"\x05\x00")
        return rx[1]


@dataclass
class ButtonState:
    line: int
    pressed: bool = False
    last_pressed: bool = False
    events: int = 0
    pulse_until: float = 0.0


class Dashboard:
    def __init__(self, args):
        self.args = args
        self.font = ImageFont.load_default()
        self.font_small = ImageFont.load_default()
        self.start = time.monotonic()
        self.next_flash_poll = 0.0
        self.flash_id = None
        self.flash_status = None
        self.flash_error = None
        self.running = True
        self.last_status_line = None
        self.next_status_print = 0.0

        self.i2c_dev = args.i2c_dev or find_dln2_i2c_dev()
        self.oled = SSD1306(self.i2c_dev, args.oled_addr)

        self.gpio_chip = args.gpio_chip
        self.button_lines = parse_int_list(args.button_lines)
        self.buttons = [ButtonState(line=line) for line in self.button_lines]
        settings = gpiod.LineSettings(direction=Direction.INPUT)
        self.gpio_request = gpiod.request_lines(
            self.gpio_chip,
            consumer="oled_flash_button_dashboard",
            config={tuple(self.button_lines): settings},
        )

        self.flash = None
        if not args.no_flash:
            spidev = args.spidev or find_dln2_spidev(args.spi_cs)
            self.flash = SpiFlash(spidev, args.spi_speed, args.spi_mode)

    def close(self):
        try:
            self.oled.clear()
        finally:
            self.oled.close()
        self.gpio_request.release()

    def stop(self, *_):
        self.running = False

    def update_buttons(self):
        now = time.monotonic()
        values = self.gpio_request.get_values(self.button_lines)
        for state, value in zip(self.buttons, values):
            high = value == Value.ACTIVE
            pressed = not high if self.args.active_low else high
            state.last_pressed = state.pressed
            state.pressed = pressed
            if pressed and not state.last_pressed:
                state.events += 1
                state.pulse_until = now + 0.18

    def update_flash(self):
        if not self.flash:
            return
        now = time.monotonic()
        if now < self.next_flash_poll:
            return
        self.next_flash_poll = now + self.args.flash_poll_s
        try:
            flash_id = self.flash.read_jedec_id()
            flash_status = self.flash.read_status()
            self.flash_id = flash_id
            self.flash_status = flash_status
            self.flash_error = None
        except OSError as exc:
            self.flash_error = exc.strerror or str(exc)
        except Exception as exc:
            self.flash_error = str(exc)

    def flash_capacity_text(self):
        if not self.flash_id or len(self.flash_id) != 3:
            return "Flash: --"
        manufacturer, mem_type, capacity_code = self.flash_id
        capacity = 1 << capacity_code if 0 < capacity_code < 32 else 0
        if capacity:
            return f"Flash:{format_bytes(capacity)}"
        return f"Flash:{manufacturer:02X}{mem_type:02X}{capacity_code:02X}"

    def draw_buttons(self, draw):
        labels = ["1", "2", "3", "4"]
        centers = [(116, 8), (116, 23), (116, 38), (116, 53)]
        now = time.monotonic()
        draw.line((102, 0, 102, 63), fill=1)
        for index, (state, center) in enumerate(zip(self.buttons, centers)):
            x, y = center
            pulse = now < state.pulse_until
            radius = 6 if state.pressed or pulse else 4
            box = (x - radius, y - radius, x + radius, y + radius)
            if state.pressed:
                draw.ellipse(box, outline=1, fill=1)
            else:
                draw.ellipse(box, outline=1, fill=0)
                if pulse:
                    draw.ellipse((x - 6, y - 6, x + 6, y + 6), outline=1)
            draw.text((104, y - 4), labels[index], font=self.font_small, fill=1)

    def draw_left_panel(self, draw):
        now_wall = time.localtime()
        uptime = int(time.monotonic() - self.start)
        event_sum = sum(button.events for button in self.buttons)

        draw.text((0, 0), time.strftime("%m-%d %a", now_wall),
                  font=self.font, fill=1)
        draw.text((58, 0), time.strftime("%H:%M:%S", now_wall),
                  font=self.font, fill=1)
        draw.text((0, 10), f"UP:{uptime // 3600:02d}:{uptime // 60 % 60:02d}:{uptime % 60:02d}",
                  font=self.font, fill=1)
        draw.text((0, 20), self.flash_capacity_text(),
                  font=self.font, fill=1)
        if self.flash_error:
            draw.text((0, 30), "SPI:ERR", font=self.font, fill=1)
        elif self.flash_id and self.flash_status is not None:
            draw.text((0, 30), f"ID:{self.flash_id.hex().upper()} S:{self.flash_status:02X}",
                      font=self.font, fill=1)
        else:
            draw.text((0, 30), "SPI:--", font=self.font, fill=1)

        draw.text((0, 40), f"Events:{event_sum}", font=self.font, fill=1)
        draw.text((0, 50), "T:--.-C H:--%", font=self.font, fill=1)

    def render(self):
        image = Image.new("1", (SSD1306_WIDTH, SSD1306_HEIGHT), 0)
        draw = ImageDraw.Draw(image)
        self.draw_left_panel(draw)
        self.draw_buttons(draw)
        self.oled.show(image)

    def status_line(self):
        states = " ".join(
            f"line{button.line}={'down' if button.pressed else 'up'}"
            for button in self.buttons
        )
        event_sum = sum(button.events for button in self.buttons)
        if self.flash_error:
            flash = f"flash_error={self.flash_error}"
        elif self.flash_id and self.flash_status is not None:
            flash = f"flash={self.flash_id.hex().upper()} status=0x{self.flash_status:02X}"
        elif self.flash:
            flash = "flash=probing"
        else:
            flash = "flash=disabled"
        return f"{states} events={event_sum} {flash}"

    def run(self):
        signal.signal(signal.SIGINT, self.stop)
        signal.signal(signal.SIGTERM, self.stop)

        frame_s = 1.0 / self.args.refresh_hz
        try:
            while self.running:
                start = time.monotonic()
                self.update_buttons()
                self.update_flash()
                self.render()
                if self.args.print_status:
                    now = time.monotonic()
                    status_line = self.status_line()
                    if (status_line != self.last_status_line or
                            now >= self.next_status_print):
                        print(status_line, flush=True)
                        self.last_status_line = status_line
                        self.next_status_print = now + self.args.print_interval_s
                if self.args.duration and time.monotonic() - self.start >= self.args.duration:
                    break
                elapsed = time.monotonic() - start
                time.sleep(max(0.0, frame_s - elapsed))
        finally:
            self.close()


def main():
    parser = argparse.ArgumentParser(
        description="OLED dashboard for ESP USB DLN2 buttons and W25Q SPI flash."
    )
    parser.add_argument("--i2c-dev", help="DLN2 i2c-dev node; auto-detected by default")
    parser.add_argument("--oled-addr", type=lambda x: int(x, 0),
                        default=SSD1306_DEFAULT_ADDR,
                        help="SSD1306 I2C address, default 0x3c")
    parser.add_argument("--gpio-chip", default=DEFAULT_GPIO_CHIP,
                        help="DLN2 GPIO chip path")
    parser.add_argument("--button-lines", default="0 1 2 3",
                        help="space/comma-separated DLN2 GPIO line offsets")
    parser.add_argument("--active-low", action=argparse.BooleanOptionalAction,
                        default=True,
                        help="buttons pull the line low when pressed")
    parser.add_argument("--spidev", help="DLN2 spidev node; auto-detected by default")
    parser.add_argument("--spi-cs", type=int, default=0)
    parser.add_argument("--spi-speed", type=int, default=100000)
    parser.add_argument("--spi-mode", type=int, default=0)
    parser.add_argument("--no-flash", action="store_true",
                        help="skip W25Q flash probing")
    parser.add_argument("--flash-poll-s", type=float, default=2.0,
                        help="seconds between flash JEDEC/status reads")
    parser.add_argument("--refresh-hz", type=float, default=8.0)
    parser.add_argument("--duration", type=float,
                        help="seconds to run; default is forever")
    parser.add_argument("--print-status", action="store_true",
                        help="also print button state lines to stdout")
    parser.add_argument("--print-interval-s", type=float, default=1.0,
                        help="maximum seconds between repeated status lines")
    args = parser.parse_args()

    if args.refresh_hz <= 0:
        raise SystemExit("--refresh-hz must be positive")
    if args.flash_poll_s <= 0:
        raise SystemExit("--flash-poll-s must be positive")
    if args.print_interval_s <= 0:
        raise SystemExit("--print-interval-s must be positive")
    if args.spi_mode not in (0, 1, 2, 3):
        raise SystemExit("--spi-mode must be 0, 1, 2, or 3")
    if len(parse_int_list(args.button_lines)) != 4:
        raise SystemExit("this dashboard layout expects exactly 4 button lines")

    dashboard = Dashboard(args)
    dashboard.run()


if __name__ == "__main__":
    try:
        main()
    except FileNotFoundError as exc:
        print(f"missing device: {exc}", file=sys.stderr)
        sys.exit(1)
    except OSError as exc:
        if exc.errno == errno.EBUSY:
            print(
                "device busy: stop other dashboard/gpiod/spidev users and retry",
                file=sys.stderr,
            )
            sys.exit(1)
        raise
