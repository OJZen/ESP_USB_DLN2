#!/usr/bin/env python3
"""ESP USB DLN2 on Windows 10/11, using only Python and system WinUSB APIs."""
import argparse
from collections import deque
import ctypes as ct
import json
import math
import os
import struct
import time
import uuid


INTERFACE_GUID = "{3D7F8D6E-A457-4DB6-9C0F-5BA0EA8D74C1}"
GPIO_PINS = (1, 2, 4, 5, 6, 7, 8, 9, 18, 21)
MAX_PACKET = 512
HEADER = struct.Struct("<HHHH")
U8, U16, U32, HANDLE = ct.c_uint8, ct.c_uint16, ct.c_uint32, ct.c_void_p


class ProtocolError(RuntimeError):
    pass


class DeviceError(RuntimeError):
    def __init__(self, command, result):
        self.command, self.result = command, result
        super().__init__(f"DLN-2 command 0x{command:04x} failed: result 0x{result:02x}")


class GUID(ct.Structure):
    _fields_ = [("data1", U32), ("data2", U16), ("data3", U16), ("data4", U8 * 8)]


class InterfaceData(ct.Structure):
    _fields_ = [("size", U32), ("guid", GUID), ("flags", U32), ("reserved", ct.c_size_t)]


class USBInterface(ct.Structure):
    _pack_ = 1
    _fields_ = [(name, U8) for name in (
        "length", "type", "number", "alternate", "endpoints", "usb_class",
        "subclass", "protocol", "string")]


class PipeInfo(ct.Structure):
    _fields_ = [("type", ct.c_int), ("id", U8), ("packet_size", U16), ("interval", U8)]


class WindowsAPI:
    def __init__(self):
        if os.name != "nt":
            raise OSError("This transport requires Windows; use the Linux host tools on Linux.")
        self.guid = GUID.from_buffer_copy(uuid.UUID(INTERFACE_GUID).bytes_le)
        self.setup = ct.WinDLL("setupapi", use_last_error=True)
        self.kernel = ct.WinDLL("kernel32", use_last_error=True)
        self.usb = ct.WinDLL("winusb", use_last_error=True)
        signatures = (
            (self.setup, "SetupDiGetClassDevsW", HANDLE, [ct.POINTER(GUID), ct.c_wchar_p, HANDLE, U32]),
            (self.setup, "SetupDiEnumDeviceInterfaces", ct.c_int,
             [HANDLE, HANDLE, ct.POINTER(GUID), U32, ct.POINTER(InterfaceData)]),
            (self.setup, "SetupDiGetDeviceInterfaceDetailW", ct.c_int,
             [HANDLE, ct.POINTER(InterfaceData), HANDLE, U32, ct.POINTER(U32), HANDLE]),
            (self.setup, "SetupDiDestroyDeviceInfoList", ct.c_int, [HANDLE]),
            (self.kernel, "CreateFileW", HANDLE,
             [ct.c_wchar_p, U32, U32, HANDLE, U32, U32, HANDLE]),
            (self.kernel, "CloseHandle", ct.c_int, [HANDLE]),
            (self.usb, "WinUsb_Initialize", ct.c_int, [HANDLE, ct.POINTER(HANDLE)]),
            (self.usb, "WinUsb_Free", ct.c_int, [HANDLE]),
            (self.usb, "WinUsb_QueryInterfaceSettings", ct.c_int,
             [HANDLE, U8, ct.POINTER(USBInterface)]),
            (self.usb, "WinUsb_QueryPipe", ct.c_int, [HANDLE, U8, U8, ct.POINTER(PipeInfo)]),
            (self.usb, "WinUsb_SetPipePolicy", ct.c_int, [HANDLE, U8, U32, U32, HANDLE]),
            (self.usb, "WinUsb_ReadPipe", ct.c_int, [HANDLE, U8, HANDLE, U32, ct.POINTER(U32), HANDLE]),
            (self.usb, "WinUsb_WritePipe", ct.c_int, [HANDLE, U8, HANDLE, U32, ct.POINTER(U32), HANDLE]),
        )
        for dll, name, result, arguments in signatures:
            function = getattr(dll, name)
            function.restype, function.argtypes = result, arguments

    @staticmethod
    def check(result):
        if not result:
            error = ct.get_last_error()
            if error == 121:
                raise TimeoutError("WinUSB transfer timed out")
            raise ct.WinError(error)

    def devices(self):
        devices = self.setup.SetupDiGetClassDevsW(ct.byref(self.guid), None, None, 0x12)
        if devices == ct.c_void_p(-1).value:
            raise ct.WinError(ct.get_last_error())
        paths = []
        try:
            index = 0
            while True:
                interface = InterfaceData()
                interface.size = ct.sizeof(interface)
                if not self.setup.SetupDiEnumDeviceInterfaces(
                        devices, None, ct.byref(self.guid), index, ct.byref(interface)):
                    if ct.get_last_error() == 259:
                        break
                    raise ct.WinError(ct.get_last_error())
                required = U32()
                self.setup.SetupDiGetDeviceInterfaceDetailW(
                    devices, ct.byref(interface), None, 0, ct.byref(required), None)
                if ct.get_last_error() != 122 or required.value < 6:
                    raise ct.WinError(ct.get_last_error())
                detail = ct.create_string_buffer(required.value)
                U32.from_buffer(detail).value = 8 if ct.sizeof(HANDLE) == 8 else 6
                self.check(self.setup.SetupDiGetDeviceInterfaceDetailW(
                    devices, ct.byref(interface), detail, len(detail), None, None))
                path = ct.wstring_at(ct.addressof(detail) + 4)
                if "#vid_a257&pid_2013#" in path.lower():
                    paths.append(path)
                index += 1
        finally:
            self.setup.SetupDiDestroyDeviceInfoList(devices)
        return sorted(paths)


class WinUSBTransport:
    def __init__(self, index=0):
        self.api = WindowsAPI()
        self.file, self.handle = None, HANDLE()
        devices = self.api.devices()
        if not devices:
            raise OSError("No ESP USB DLN2 WinUSB interface found. Flash the Windows-enabled "
                          "firmware and unplug/replug native USB; see WINDOWS.md.")
        if not 0 <= index < len(devices):
            raise ValueError(f"device index must be 0..{len(devices) - 1}")
        self.path = devices[index]
        self.file = self.api.kernel.CreateFileW(
            self.path, 0xC0000000, 3, None, 3, 0x40000000, None)
        if self.file == ct.c_void_p(-1).value:
            self.file = None
            raise ct.WinError(ct.get_last_error())
        try:
            self.api.check(self.api.usb.WinUsb_Initialize(self.file, ct.byref(self.handle)))
            interface = USBInterface()
            self.api.check(self.api.usb.WinUsb_QueryInterfaceSettings(
                self.handle, 0, ct.byref(interface)))
            if interface.number != 0 or interface.usb_class != 0xFF:
                raise ProtocolError("Unexpected DLN-2 USB interface")
            pipes = set()
            for index in range(interface.endpoints):
                pipe = PipeInfo()
                self.api.check(self.api.usb.WinUsb_QueryPipe(self.handle, 0, index, ct.byref(pipe)))
                if pipe.type == 2:
                    pipes.add(pipe.id)
            if pipes != {0x01, 0x81}:
                raise ProtocolError(f"Unexpected DLN-2 bulk endpoints: {pipes}")
            self._timeout(0x01, 1000)
        except BaseException:
            self.close()
            raise

    def _timeout(self, pipe, milliseconds):
        timeout = U32(milliseconds)
        self.api.check(self.api.usb.WinUsb_SetPipePolicy(
            self.handle, pipe, 3, ct.sizeof(timeout), ct.byref(timeout)))

    def write(self, data):
        buffer = ct.create_string_buffer(data)
        written = U32()
        self.api.check(self.api.usb.WinUsb_WritePipe(
            self.handle, 0x01, buffer, len(data), ct.byref(written), None))
        return written.value

    def read(self, timeout_ms):
        self._timeout(0x81, timeout_ms)
        buffer = ct.create_string_buffer(MAX_PACKET)
        received = U32()
        self.api.check(self.api.usb.WinUsb_ReadPipe(
            self.handle, 0x81, buffer, len(buffer), ct.byref(received), None))
        return buffer.raw[:received.value]

    def close(self):
        if self.handle:
            self.api.usb.WinUsb_Free(self.handle)
            self.handle = HANDLE()
        if self.file is not None:
            self.api.kernel.CloseHandle(self.file)
            self.file = None


class DLN2:
    """One synchronous owner per adapter. Events can be consumed with next_event()."""
    def __init__(self, transport):
        self.transport = transport
        self.echo = 0
        self.buffer = bytearray()
        self.events = deque()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.transport.close()

    def _frame(self, deadline):
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("DLN-2 response deadline expired")
            if len(self.buffer) >= 2:
                size = struct.unpack_from("<H", self.buffer)[0]
                if not HEADER.size <= size <= MAX_PACKET:
                    raise ProtocolError(f"Invalid DLN-2 frame length: {size}")
                if len(self.buffer) >= size:
                    frame = bytes(self.buffer[:size])
                    del self.buffer[:size]
                    return frame
            data = self.transport.read(max(1, math.ceil(remaining * 1000)))
            if not data:
                raise ProtocolError("Empty DLN-2 USB transfer")
            self.buffer.extend(data)

    def request(self, handle, command, payload=b"", timeout=1.0):
        if not 1 <= handle <= 5 or not 0 <= command <= 0xFFFF:
            raise ValueError("invalid DLN-2 handle or command")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be positive and finite")
        payload = bytes(payload)
        size = HEADER.size + len(payload)
        if size > MAX_PACKET:
            raise ValueError("DLN-2 request exceeds 512 bytes")
        self.echo = (self.echo + 1) & 0xFFFF
        packet = HEADER.pack(size, command, self.echo, handle) + payload
        deadline = time.monotonic() + timeout
        if self.transport.write(packet) != len(packet):
            raise ProtocolError("Short DLN-2 USB write")
        while True:
            frame = self._frame(deadline)
            _, received_command, echo, received_handle = HEADER.unpack_from(frame)
            if received_handle == 0:
                if len(self.events) >= 256:
                    raise ProtocolError("DLN-2 event queue full; consume events with next_event()")
                self.events.append((received_command, echo, frame[8:]))
                continue
            if (received_command, echo, received_handle) != (command, self.echo, handle):
                raise ProtocolError("DLN-2 response does not match request; reopen the device")
            if len(frame) < 10:
                raise ProtocolError("DLN-2 response is missing its result code")
            result = struct.unpack_from("<H", frame, 8)[0]
            if result != 0:
                raise DeviceError(command, result)
            return frame[10:]

    def next_event(self, timeout=1.0):
        if self.events:
            return self.events.popleft()
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be positive and finite")
        frame = self._frame(time.monotonic() + timeout)
        _, command, echo, handle = HEADER.unpack_from(frame)
        if handle != 0:
            raise ProtocolError("Expected DLN-2 event; reopen the device")
        return command, echo, frame[8:]

    @staticmethod
    def _unpack(fmt, payload):
        if len(payload) != struct.calcsize(fmt):
            raise ProtocolError(f"Unexpected response payload length: {len(payload)}")
        return struct.unpack(fmt, payload)

    @staticmethod
    def _data(payload, length):
        if len(payload) != length + 2 or struct.unpack_from("<H", payload)[0] != length:
            raise ProtocolError("DLN-2 transfer length does not match requested length")
        return payload[2:]

    def info(self):
        version, = self._unpack("<I", self.request(1, 0x0030))
        if version != 0x200:
            raise ProtocolError(f"Unexpected DLN-2 hardware ID: 0x{version:08x}")
        serial, = self._unpack("<I", self.request(1, 0x0031))
        gpio_count, = self._unpack("<H", self.request(2, 0x0101))
        adc_count, = self._unpack("<B", self.request(5, 0x0601, b"\0"))
        cs_count, = self._unpack("<H", self.request(4, 0x0244, b"\0"))
        spi_min, = self._unpack("<I", self.request(4, 0x0245, b"\0"))
        spi_max, = self._unpack("<I", self.request(4, 0x0246, b"\0"))
        return {"hardware_id": f"0x{version:08x}", "serial": f"{serial:08x}",
                "gpio_lines": gpio_count, "adc_channels": adc_count,
                "spi_chip_selects": cs_count, "spi_hz": [spi_min, spi_max]}

    @staticmethod
    def _pin(pin):
        if not 0 <= pin < len(GPIO_PINS):
            raise ValueError("GPIO line must be 0..9 (see GPIO_PINS for ESP32 pin numbers)")
        return struct.pack("<H", pin)

    def gpio_read(self, pin):
        data = self._pin(pin)
        self.request(2, 0x0110, data)
        try:
            echoed_pin, value = self._unpack("<HB", self.request(2, 0x010B, data))
            if echoed_pin != pin or value not in (0, 1):
                raise ProtocolError("Invalid GPIO read response")
            return value
        finally:
            self.request(2, 0x0111, data)

    def gpio_write(self, pin, value):
        data = self._pin(pin)
        if value not in (0, 1):
            raise ValueError("GPIO value must be 0 or 1")
        self.request(2, 0x0110, data)
        try:
            self.request(2, 0x010C, data + bytes([value]))
            self.request(2, 0x0113, data + b"\1")
        except BaseException:
            self.request(2, 0x0111, data)
            raise

    def gpio_release(self, pin):
        self.request(2, 0x0111, self._pin(pin))

    @staticmethod
    def _i2c_header(address, length):
        if not 0 <= address <= 0x7F or not 0 <= length <= 256:
            raise ValueError("I2C needs a 7-bit address and 0..256 bytes")
        return struct.pack("<BBBIH", 0, address, 0, 0, length)

    def i2c_write(self, address, data):
        data = bytes(data)
        payload = self._i2c_header(address, len(data)) + data
        self.request(3, 0x0301, b"\0")
        try:
            self.request(3, 0x0306, payload)
        finally:
            self.request(3, 0x0302, b"\0")

    def i2c_read(self, address, length):
        payload = self._i2c_header(address, length)
        self.request(3, 0x0301, b"\0")
        try:
            return self._data(self.request(3, 0x0307, payload), length)
        finally:
            self.request(3, 0x0302, b"\0")

    def spi_transfer(self, data, speed=1000000, mode=0):
        data = bytes(data)
        if not 1 <= len(data) <= 256 or not 10000 <= speed <= 10000000 or mode not in range(4):
            raise ValueError("SPI requires 1..256 bytes, 10000..10000000 Hz, mode 0..3")
        self.request(4, 0x0238, b"\0\1")
        try:
            self.request(4, 0x0214, bytes([0, mode]))
            self.request(4, 0x0216, b"\0\x08")
            self.request(4, 0x0218, struct.pack("<BI", 0, speed))
            self.request(4, 0x0226, b"\0\xfe")
            self.request(4, 0x0211, b"\0")
            try:
                return self._data(self.request(4, 0x021A,
                                              struct.pack("<BHB", 0, len(data), 0) + data), len(data))
            finally:
                self.request(4, 0x0212, b"\0\0")
        finally:
            self.request(4, 0x0239, b"\0\1")

    def adc_read(self, channel):
        if channel not in range(4):
            raise ValueError("ADC channel must be 0..3")
        data = bytes([0, channel])
        self.request(5, 0x0602, b"\0")
        try:
            self.request(5, 0x0608, b"\0\x0a")
            self.request(5, 0x0605, data)
            try:
                value, = self._unpack("<H", self.request(5, 0x060A, data))
                if value > 1023:
                    raise ProtocolError("ADC raw value exceeds 10-bit range")
                return value
            finally:
                self.request(5, 0x0606, data)
        finally:
            self.request(5, 0x0603, b"\0")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index", type=int, default=0, help="device index from 'devices'")
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("devices", help="list Windows-enabled adapter paths")
    commands.add_parser("check", help="read identity and capabilities without driving pins")
    for command in ("gpio-read", "gpio-write", "gpio-release"):
        child = commands.add_parser(command)
        child.add_argument("pin", type=int, help="DLN-2 line offset 0..9, not ESP32 GPIO number")
        if command == "gpio-write":
            child.add_argument("value", type=int, choices=(0, 1))
    for command in ("i2c-read", "i2c-write"):
        child = commands.add_parser(command)
        child.add_argument("address", type=lambda s: int(s, 0), help="7-bit address, e.g. 0x3c")
        child.add_argument("length" if command == "i2c-read" else "data",
                           type=int if command == "i2c-read" else bytes.fromhex,
                           help="byte count" if command == "i2c-read" else 'hex bytes, e.g. "00 af"; "" probes')
    child = commands.add_parser("spi", help="full-duplex SPI transfer on CS0")
    child.add_argument("data", type=bytes.fromhex, help='hex bytes, e.g. "9f 00 00 00"')
    child.add_argument("--speed", type=int, default=1000000)
    child.add_argument("--mode", type=int, default=0, choices=range(4))
    child = commands.add_parser("adc", help="read a 10-bit raw sample")
    child.add_argument("channel", type=int, choices=range(4))
    args = parser.parse_args()
    try:
        if args.command == "devices":
            print(json.dumps(WindowsAPI().devices(), indent=2))
            return
        with DLN2(WinUSBTransport(args.index)) as device:
            if args.command in (None, "check"):
                print(json.dumps(device.info(), indent=2))
            elif args.command == "gpio-read":
                print(device.gpio_read(args.pin))
            elif args.command == "gpio-write":
                device.gpio_write(args.pin, args.value)
                print("OK (output held; use gpio-release to return to input)")
            elif args.command == "gpio-release":
                device.gpio_release(args.pin)
                print("OK")
            elif args.command == "i2c-read":
                print(device.i2c_read(args.address, args.length).hex(" "))
            elif args.command == "i2c-write":
                device.i2c_write(args.address, args.data)
                print("OK")
            elif args.command == "spi":
                print(device.spi_transfer(args.data, args.speed, args.mode).hex(" "))
            elif args.command == "adc":
                print(device.adc_read(args.channel))
    except (OSError, RuntimeError, ValueError) as exc:
        parser.exit(1, f"ERROR: {exc}\n")


if __name__ == "__main__":
    main()
