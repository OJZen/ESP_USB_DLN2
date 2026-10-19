import ctypes
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from dln2_windows import DLN2, DeviceError, GUID, HEADER, InterfaceData, ProtocolError


def response(command=0x30, echo=1, handle=1, payload=b"", result=0):
    return HEADER.pack(10 + len(payload), command, echo, handle) + struct.pack("<H", result) + payload


class Transport:
    def __init__(self, responder=None, chunks=()):
        self.responder = responder
        self.chunks = list(chunks)
        self.writes = []
        self.closed = False

    def write(self, packet):
        self.writes.append(packet)
        if self.responder:
            size, command, echo, handle = HEADER.unpack_from(packet)
            assert size == len(packet)
            result, payload = self.responder(handle, command, packet[8:])
            self.chunks.append(response(command, echo, handle, payload, result))
        return len(packet)

    def read(self, timeout_ms):
        assert timeout_ms > 0
        if not self.chunks:
            raise TimeoutError("test timeout")
        return self.chunks.pop(0)

    def close(self):
        self.closed = True

    def commands(self):
        return [(HEADER.unpack_from(p)[1], p[8:]) for p in self.writes]


class WindowsSupportTests(unittest.TestCase):
    def test_packet_and_fragmented_response(self):
        frame = response(payload=b"\x00\x02\x00\x00")
        transport = Transport(chunks=[frame[:1], frame[1:7], frame[7:]])
        with DLN2(transport) as device:
            self.assertEqual(device.request(1, 0x30), b"\x00\x02\x00\x00")
        self.assertEqual(transport.writes, [b"\x08\x00\x30\x00\x01\x00\x01\x00"])
        self.assertTrue(transport.closed)

    def test_coalesced_event_and_response(self):
        event = HEADER.pack(10, 0x010F, 99, 0) + b"ab"
        device = DLN2(Transport(chunks=[event + response(payload=b"ok")]))
        self.assertEqual(device.request(1, 0x30), b"ok")
        self.assertEqual(device.next_event(), (0x010F, 99, b"ab"))

    def test_event_cannot_extend_deadline(self):
        event = HEADER.pack(8, 0x010F, 1, 0)
        device = DLN2(Transport(chunks=[event, event]))
        with patch("dln2_windows.time.monotonic", side_effect=[0, 0.05, 0.11]):
            with self.assertRaises(TimeoutError):
                device.request(1, 0x30, timeout=0.1)

    def test_event_queue_is_bounded(self):
        event = HEADER.pack(8, 0x010F, 1, 0)
        device = DLN2(Transport(chunks=[event * 257]))
        with self.assertRaisesRegex(ProtocolError, "queue full"):
            device.request(1, 0x30)
        self.assertEqual(len(device.events), 256)

    def test_next_event_reads_usb(self):
        device = DLN2(Transport(chunks=[HEADER.pack(9, 0x0610, 0, 0) + b"x"]))
        self.assertEqual(device.next_event(), (0x0610, 0, b"x"))

    def test_invalid_frame_lengths_and_missing_result(self):
        for frame in (b"\x07\0", b"\x01\x02", HEADER.pack(8, 0x30, 1, 1), b""):
            with self.subTest(frame=frame), self.assertRaises(ProtocolError):
                DLN2(Transport(chunks=[frame])).request(1, 0x30)

    def test_response_must_match_command_echo_and_handle(self):
        for kwargs in ({"command": 0x31}, {"echo": 2}, {"handle": 2}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ProtocolError, "match"):
                DLN2(Transport(chunks=[response(**kwargs)])).request(1, 0x30)

    def test_device_error_retains_result(self):
        with self.assertRaises(DeviceError) as error:
            DLN2(Transport(chunks=[response(result=0x91)])).request(1, 0x30)
        self.assertEqual((error.exception.command, error.exception.result), (0x30, 0x91))

    def test_short_write(self):
        transport = Transport()
        transport.write = lambda packet: len(packet) - 1
        with self.assertRaisesRegex(ProtocolError, "Short"):
            DLN2(transport).request(1, 0x30)

    def test_request_validation_before_usb(self):
        transport = Transport()
        device = DLN2(transport)
        for args, kwargs in (((0, 0x30), {}), ((1, 0x10000), {}),
                             ((1, 0x30, bytes(505)), {}), ((1, 0x30), {"timeout": 0}),
                             ((1, 0x30), {"timeout": float("nan")})):
            with self.subTest(args=args), self.assertRaises(ValueError):
                device.request(*args, **kwargs)
        self.assertEqual(transport.writes, [])

    def test_echo_wrap(self):
        device = DLN2(Transport(chunks=[response(echo=0)]))
        device.echo = 65535
        self.assertEqual(device.request(1, 0x30), b"")

    def test_check_reads_capabilities_without_enabling_hardware(self):
        payloads = {0x30: struct.pack("<I", 0x200), 0x31: struct.pack("<I", 0x12345678),
                    0x101: b"\x0a\0", 0x601: b"\4", 0x244: b"\1\0",
                    0x245: struct.pack("<I", 10000), 0x246: struct.pack("<I", 10000000)}
        transport = Transport(lambda handle, command, data: (0, payloads[command]))
        info = DLN2(transport).info()
        self.assertEqual(info["serial"], "12345678")
        self.assertEqual((info["gpio_lines"], info["adc_channels"]), (10, 4))
        self.assertEqual({cmd for cmd, _ in transport.commands()}, set(payloads))

    def test_gpio_read_releases_input_on_error(self):
        transport = Transport(lambda h, command, p: (0x83 if command == 0x10B else 0, p))
        with self.assertRaises(DeviceError):
            DLN2(transport).gpio_read(2)
        self.assertEqual(transport.commands()[-1], (0x111, b"\2\0"))

    def test_gpio_read_checks_echoed_pin(self):
        transport = Transport(lambda h, cmd, p: (0, b"\3\0\1" if cmd == 0x10B else b""))
        with self.assertRaisesRegex(ProtocolError, "GPIO"):
            DLN2(transport).gpio_read(2)
        self.assertEqual(transport.commands()[-1][0], 0x111)

    def test_gpio_output_sets_latch_before_direction_and_explicit_release(self):
        transport = Transport(lambda *args: (0, b""))
        device = DLN2(transport)
        device.gpio_write(0, 1)
        self.assertEqual(transport.commands(), [(0x110, b"\0\0"),
                                               (0x10C, b"\0\0\1"), (0x113, b"\0\0\1")])
        device.gpio_release(0)
        self.assertEqual(transport.commands()[-1], (0x111, b"\0\0"))

    def test_i2c_header_and_maximum_transfer(self):
        data = bytes(range(256))
        transport = Transport(lambda h, cmd, p: (0, b"\0\1" + data if cmd == 0x307 else b""))
        device = DLN2(transport)
        device.i2c_write(0x52, data)
        self.assertEqual(transport.commands()[1], (0x306, b"\0\x52\0\0\0\0\0\0\1" + data))
        self.assertEqual(device.i2c_read(0x52, 256), data)
        self.assertEqual(transport.commands()[-1], (0x302, b"\0"))

    def test_i2c_failure_disables_bus(self):
        transport = Transport(lambda h, cmd, p: (0xBA if cmd == 0x307 else 0, b""))
        with self.assertRaises(DeviceError):
            DLN2(transport).i2c_read(0x52, 1)
        self.assertEqual(transport.commands()[-1], (0x302, b"\0"))

    def test_spi_cs_ownership_polarity_and_release(self):
        transport = Transport(lambda h, cmd, p: (0, b"\4\0\x00\xef\x40\x16" if cmd == 0x21A else b""))
        result = DLN2(transport).spi_transfer(b"\x9f\0\0\0", 100000, 3)
        self.assertEqual(result, b"\0\xef\x40\x16")
        calls = transport.commands()
        self.assertEqual(calls[0], (0x238, b"\0\1"))
        self.assertLess([c for c, _ in calls].index(0x238), [c for c, _ in calls].index(0x211))
        self.assertIn((0x226, b"\0\xfe"), calls)
        self.assertIn((0x21A, b"\0\4\0\0\x9f\0\0\0"), calls)
        self.assertEqual(calls[-2:], [(0x212, b"\0\0"), (0x239, b"\0\1")])

    def test_spi_configuration_failure_releases_cs(self):
        transport = Transport(lambda h, cmd, p: (0x83 if cmd == 0x214 else 0, b""))
        with self.assertRaises(DeviceError):
            DLN2(transport).spi_transfer(b"\0")
        self.assertEqual(transport.commands()[-1], (0x239, b"\0\1"))

    def test_adc_value_and_cleanup(self):
        transport = Transport(lambda h, cmd, p: (0, b"\xff\3" if cmd == 0x60A else b""))
        self.assertEqual(DLN2(transport).adc_read(3), 1023)
        self.assertEqual(transport.commands()[-2:], [(0x606, b"\0\3"), (0x603, b"\0")])

    def test_adc_invalid_value_still_releases_hardware(self):
        transport = Transport(lambda h, cmd, p: (0, b"\0\4" if cmd == 0x60A else b""))
        with self.assertRaises(ProtocolError):
            DLN2(transport).adc_read(0)
        self.assertEqual(transport.commands()[-2:], [(0x606, b"\0\0"), (0x603, b"\0")])

    def test_invalid_io_arguments_never_touch_usb(self):
        transport = Transport()
        device = DLN2(transport)
        for method, args in ((device.gpio_read, (10,)), (device.gpio_write, (0, 2)),
                             (device.i2c_read, (0xA4, 1)), (device.i2c_read, (0x52, 257)),
                             (device.spi_transfer, (b"",)), (device.adc_read, (4,))):
            with self.subTest(method=method.__name__), self.assertRaises(ValueError):
                method(*args)
        self.assertEqual(transport.writes, [])

    def test_transfer_response_count_must_match(self):
        for payload in (b"", b"\1", b"\2\0x", b"\1\0xx"):
            with self.subTest(payload=payload), self.assertRaises(ProtocolError):
                DLN2._data(payload, 1)

    def test_native_structures_keep_pointer_width(self):
        self.assertEqual(ctypes.sizeof(GUID), 16)
        self.assertEqual(ctypes.sizeof(InterfaceData), 32 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)


if __name__ == "__main__":
    unittest.main()
