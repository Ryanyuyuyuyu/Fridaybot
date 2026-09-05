#!/usr/bin/env python3
"""Verify linked USB descriptors in an ESP32-S3 ELF, without executing it."""

from pathlib import Path
import struct
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def linked_objects(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    require(data[:6] == b"\x7fELF\x01\x01", "expected a little-endian ELF32 firmware")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    section_offset, section_size, section_count = header[6], header[11], header[12]
    require(section_size == 40 and section_count > 0, "unsupported ELF section table")
    sections = [struct.unpack_from("<10I", data, section_offset + i * section_size)
                for i in range(section_count)]
    objects = {}
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        require(section[9] == 16, "unexpected ELF32 symbol size")
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for offset in range(section[4], section[4] + section[5], 16):
            name_at, address, size, info, _other, index = struct.unpack_from("<IIIBBH", data, offset)
            if info & 15 != 1 or not (0 < index < len(sections)):
                continue
            end = names.find(b"\0", name_at)
            name = names[name_at:end].decode("ascii")
            if "codex_micro3usb" not in name:
                continue
            for key in ("configurationDescriptor", "kReportDescriptor", "deviceDescriptor"):
                if not name.endswith(f"{len(key)}{key}E"):
                    continue
                owner = sections[index]
                relative = address - owner[3]
                require(0 <= relative and relative + size <= owner[5], f"{key} lies outside its ELF section")
                contents = data[owner[4] + relative:owner[4] + relative + size]
                require(len(contents) == size and size > 0, f"{key} has no linked bytes")
                require(key not in objects, f"ambiguous linked symbol {key}")
                objects[key] = contents
    require(len(objects) == 3, "missing USB descriptors; use the unstripped USB-enabled firmware ELF")
    return objects


def descriptor_items(data: bytes) -> list[bytes]:
    items = []
    offset = 0
    while offset < len(data):
        size = data[offset]
        require(size >= 2 and offset + size <= len(data), "malformed USB descriptor boundary")
        items.append(data[offset:offset + size])
        offset += size
    return items


def report_bits(data: bytes) -> dict[tuple[int, int], int]:
    bits = {}
    offset = 0
    report_id = size = count = 0
    while offset < len(data):
        prefix = data[offset]
        offset += 1
        require(prefix != 0xfe, "unexpected HID long item")
        length = (0, 1, 2, 4)[prefix & 3]
        require(offset + length <= len(data), "truncated HID report descriptor")
        value = int.from_bytes(data[offset:offset + length], "little")
        offset += length
        item_type, tag = (prefix >> 2) & 3, prefix >> 4
        if item_type == 1:
            if tag == 7:
                size = value
            elif tag == 8:
                report_id = value
            elif tag == 9:
                count = value
        elif item_type == 0 and tag in (8, 9, 11):
            key = (report_id, tag)
            bits[key] = bits.get(key, 0) + size * count
    return bits


def verify(path: Path) -> None:
    objects = linked_objects(path)
    configuration = objects["configurationDescriptor"]
    items = descriptor_items(configuration)
    interfaces = [item for item in items if item[1] == 4]
    endpoints = [item for item in items if item[1] == 5]
    require(len(endpoints) == 1, f"expected one interrupt IN endpoint; found {len(endpoints)} endpoints")
    require([item[1] for item in items] == [2, 4, 0x21, 5], "unexpected configuration descriptor sequence")
    config, interface, hid, endpoint = items
    require(len(config) == 9 and len(configuration) == 34, "expected a 34-byte IN-only configuration")
    require(int.from_bytes(config[2:4], "little") == len(configuration), "wTotalLength does not match linked bytes")
    require(config[4:6] == b"\x01\x01" and len(interfaces) == 1, "expected one configuration and interface")
    require(len(interface) == 9 and interface[2:8] == b"\x00\x00\x01\x03\x00\x00",
            "expected interface 0, one endpoint, HID with no boot protocol")
    require(len(endpoint) == 7 and endpoint[2:] == b"\x81\x03\x40\x00\x01",
            "expected interrupt IN 0x81, 64-byte packets, 1-ms interval; OUT must be absent")
    report = objects["kReportDescriptor"]
    require(len(hid) == 9 and hid[5:7] == b"\x01\x22" and
            int.from_bytes(hid[7:9], "little") == len(report), "HID report descriptor length mismatch")
    require(report_bits(report) == {(6, 8): 63 * 8, (6, 9): 63 * 8,
                                   (7, 11): 128 * 8, (8, 11): 256 * 8},
            "RPC Input/Output or identity/quota Feature reports changed")
    device = objects["deviceDescriptor"]
    require(len(device) == 18 and device[7] == 64, "expected an 18-byte device descriptor and 64-byte EP0")
    require(struct.unpack_from("<HHH", device, 8) == (0x303a, 0x8360, 0x0100), "device identity/release changed")
    print("usb_configuration_descriptor_test: PASS (linked ELF: IN-only 0x81/64, EP0 Output 6 + Feature 7/8)")


if __name__ == "__main__":
    try:
        require(len(sys.argv) == 2, "usage: usb_configuration_descriptor_test.py firmware.elf")
        verify(Path(sys.argv[1]))
    except (OSError, ValueError, IndexError, struct.error) as error:
        print(f"usb_configuration_descriptor_test: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
