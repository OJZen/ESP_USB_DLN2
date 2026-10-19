"""Check the actual firmware ELF descriptors using ESP-IDF's pyelftools dependency."""
from pathlib import Path
import struct
import sys

from elftools.elf.elffile import ELFFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from dln2_windows import INTERFACE_GUID


def check(path):
    with open(path, "rb") as source:
        elf = ELFFile(source)
        symbols = elf.get_section_by_name(".symtab")

        def data(name):
            matches = symbols.get_symbol_by_name(name)
            assert matches and len(matches) == 1, f"missing/ambiguous symbol: {name}"
            symbol = matches[0]
            section = elf.get_section(symbol["st_shndx"])
            offset = symbol["st_value"] - section["sh_addr"]
            return section.data()[offset:offset + symbol["st_size"]]

        device = data("dln2_device_descriptor")
        assert len(device) == 18
        assert struct.unpack_from("<H", device, 2)[0] == 0x0210
        assert struct.unpack_from("<HHH", device, 8) == (0xA257, 0x2013, 0x0101)

        config = data("dln2_fs_configuration_descriptor")
        assert struct.unpack_from("<H", config, 2)[0] == len(config)
        assert config[4:6] == b"\1\1", "one configuration with one interface"
        descriptors = []
        offset = 0
        while offset < len(config):
            size = config[offset]
            assert size >= 2 and offset + size <= len(config)
            descriptors.append(config[offset:offset + size])
            offset += size
        interface, = [d for d in descriptors if d[1] == 4]
        assert interface[2:8] == b"\0\0\2\xff\0\0", "Linux interface must stay intact"
        endpoints = [d for d in descriptors if d[1] == 5]
        assert len(endpoints) == 2
        assert {(d[2], d[3], struct.unpack_from("<H", d, 4)[0]) for d in endpoints} == {
            (0x01, 2, 64), (0x81, 2, 64)}

        bos = data("dln2_bos_descriptor")
        assert bos[:2] == b"\5\x0f" and len(bos) == 33
        assert struct.unpack_from("<H", bos, 2)[0] == len(bos) and bos[4] == 1
        assert bos[5:9] == b"\x1c\x10\5\0"
        assert bos[9:25] == bytes.fromhex("df60ddd88945c74c9cd2659d9e648a9f")
        assert struct.unpack_from("<I", bos, 25)[0] == 0x06030000

        ms = data("dln2_ms_os_20_descriptor")
        assert len(ms) == 162
        assert struct.unpack_from("<HHIH", ms) == (10, 0, 0x06030000, len(ms))
        # Single-interface devices need device-level features, without composite subsets.
        assert struct.unpack_from("<HH", ms, 10) == (20, 3)
        assert ms[14:30] == b"WINUSB\0\0" + bytes(8)
        assert struct.unpack_from("<HHHH", ms, 30) == (132, 4, 7, 42)
        assert ms[38:80].decode("utf-16-le") == "DeviceInterfaceGUIDs\0"
        assert struct.unpack_from("<H", ms, 80)[0] == 80
        assert ms[82:].decode("utf-16-le") == INTERFACE_GUID + "\0\0"
        assert struct.unpack_from("<HBB", bos, 29) == (len(ms), 0x20, 0)

    print("PASS: Linux interface/endpoints preserved; BOS, WinUSB ID and interface GUID valid")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: python tests/check_usb_descriptors.py path/to/esp_usb_dln2.elf")
    check(sys.argv[1])
