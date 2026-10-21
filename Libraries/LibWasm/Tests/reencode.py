#!/usr/bin/env python3
# Re-encode the function bodies of WebAssembly modules, so that the engine's decoding of immediates can be tested on
# every encoding the binary format allows, not only the minimal ones that tools emit.
#
#   reencode.py --mode wide IN OUT       Every LEB128 in a function body takes its maximum width: 5 bytes for u32,
#                                        s32, and s33, 10 for u64 and s64.
#   reencode.py --mode memory-index ...  Every memory argument names its memory, including memory 0.
#   reencode.py --tree SOURCE DEST ...   Re-encode every .wasm under SOURCE into DEST; other files are linked.
#
# Modules this script cannot decode (malformed or invalid ones that the tests expect to be rejected) are copied as
# they are. Sections and bodies get their sizes fixed up.

import argparse
import os
import sys


class DecodeError(Exception):
    pass


class Reader:
    def __init__(self, data, position=0, end=None):
        self.data = data
        self.position = position
        self.end = len(data) if end is None else end

    def byte(self):
        if self.position >= self.end:
            raise DecodeError("unexpected end")
        value = self.data[self.position]
        self.position += 1
        return value

    def bytes(self, count):
        if self.position + count > self.end:
            raise DecodeError("unexpected end")
        value = self.data[self.position : self.position + count]
        self.position += count
        return value

    def unsigned(self, bits):
        # Strict, like the binary format: at most ceil(bits / 7) bytes, and the unused bits of the last one are zero.
        result = 0
        shift = 0
        while True:
            byte = self.byte()
            result |= (byte & 0x7F) << shift
            shift += 7
            if byte & 0x80 == 0:
                break
            if shift >= bits:
                raise DecodeError("integer too long")
        if result >> bits:
            raise DecodeError("integer too large")
        return result

    def signed(self, bits):
        # Strict, like the binary format: at most ceil(bits / 7) bytes, and the unused bits of the last one extend the
        # sign.
        result = 0
        shift = 0
        while True:
            byte = self.byte()
            result |= (byte & 0x7F) << shift
            shift += 7
            if byte & 0x80 == 0:
                break
            if shift >= bits:
                raise DecodeError("integer too long")
        if shift > bits:
            unused = shift - bits
            top = (byte & 0x7F) >> (7 - unused - 1)
            if top not in (0, (1 << (unused + 1)) - 1):
                raise DecodeError("integer too large")
        if result >> (shift - 1):
            result -= 1 << shift
        return result


def unsigned_leb(value, width=None):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value == 0 and width is None:
            out.append(byte)
            return bytes(out)
        out.append(byte | 0x80)
        if width is not None and len(out) == width - 1:
            out.append(value & 0x7F)
            return bytes(out)


def signed_leb(value, width=None):
    out = bytearray()
    if width is None:
        while True:
            byte = value & 0x7F
            value >>= 7
            if (value == 0 and byte & 0x40 == 0) or (value == -1 and byte & 0x40):
                out.append(byte)
                return bytes(out)
            out.append(byte | 0x80)
    for _ in range(width - 1):
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value & 0x7F)
    return bytes(out)


# Widths of the maximal encodings.
U32 = 5
S32 = 5
S33 = 5
U64 = 10
S64 = 10


class Encoder:
    def __init__(self, mode):
        self.mode = mode
        self.out = bytearray()

    def u32(self, value):
        self.out += unsigned_leb(value, U32 if self.mode == "wide" else None)

    def u64(self, value):
        self.out += unsigned_leb(value, U64 if self.mode == "wide" else None)

    def s32(self, value):
        self.out += signed_leb(value, S32 if self.mode == "wide" else None)

    def s33(self, value):
        self.out += signed_leb(value, S33 if self.mode == "wide" else None)

    def s64(self, value):
        self.out += signed_leb(value, S64 if self.mode == "wide" else None)

    def raw(self, data):
        self.out += data


def copy_heap_type(reader, encoder):
    first = reader.data[reader.position]
    if first & 0x40 and first & 0x80 == 0:
        # An abstract heap type, which is a byte.
        encoder.raw(bytes([reader.byte()]))
        return
    value = reader.signed(33)
    if value < 0:
        raise DecodeError("negative heap type")
    encoder.s33(value)


def copy_value_type(reader, encoder):
    byte = reader.byte()
    encoder.raw(bytes([byte]))
    if byte in (0x63, 0x64):
        copy_heap_type(reader, encoder)


def copy_block_type(reader, encoder):
    first = reader.data[reader.position]
    if first == 0x40:
        encoder.raw(bytes([reader.byte()]))
        return
    if first & 0x40 and first & 0x80 == 0:
        copy_value_type(reader, encoder)
        return
    value = reader.signed(33)
    if value < 0:
        raise DecodeError("negative block type")
    encoder.s33(value)


def copy_memory_argument(reader, encoder):
    flags = reader.unsigned(32)
    has_index = flags & 0x40
    memory = reader.unsigned(32) if has_index else 0
    offset = reader.unsigned(64)
    if encoder.mode == "memory-index":
        has_index = True
    encoder.u32(flags | 0x40 if has_index else flags)
    if has_index:
        encoder.u32(memory)
    encoder.u64(offset)


# Immediates of the single-byte opcodes. "u" is a u32, "b" a block type, "m" a memory argument, "h" a heap type, "l" a
# lane index byte, "z" a zero byte, and "v" sixteen bytes.
PLAIN = {
    0x00: "",
    0x01: "",
    0x02: "b",
    0x03: "b",
    0x04: "b",
    0x05: "",
    0x06: "b",
    0x07: "u",
    0x08: "u",
    0x09: "u",
    0x0A: "",
    0x0B: "",
    0x0C: "u",
    0x0D: "u",
    0x0F: "",
    0x10: "u",
    0x11: "uu",
    0x12: "u",
    0x13: "uu",
    0x14: "u",
    0x15: "u",
    0x18: "u",
    0x19: "",
    0x1A: "",
    0x1B: "",
    0x20: "u",
    0x21: "u",
    0x22: "u",
    0x23: "u",
    0x24: "u",
    0x25: "u",
    0x26: "u",
    0x3F: "u",
    0x40: "u",
    0xD0: "h",
    0xD1: "",
    0xD2: "u",
    0xD3: "",
    0xD4: "",
    0xD5: "u",
    0xD6: "u",
}
for opcode in range(0x28, 0x3F):
    PLAIN[opcode] = "m"
for opcode in range(0x45, 0xC5):
    PLAIN[opcode] = ""

# The 0xFB (GC) sub-opcodes. "c" is br_on_cast's flags byte and two heap types after a label.
GC = {
    0: "u",
    1: "u",
    2: "uu",
    3: "uu",
    4: "uu",
    5: "uu",
    6: "u",
    7: "u",
    8: "uu",
    9: "uu",
    10: "uu",
    11: "u",
    12: "u",
    13: "u",
    14: "u",
    15: "",
    16: "u",
    17: "uu",
    18: "uu",
    19: "uu",
    20: "h",
    21: "h",
    22: "h",
    23: "h",
    24: "c",
    25: "c",
    26: "",
    27: "",
    28: "",
    29: "",
    30: "",
}

MISC = {
    0: "",
    1: "",
    2: "",
    3: "",
    4: "",
    5: "",
    6: "",
    7: "",
    8: "uu",
    9: "u",
    10: "uu",
    11: "u",
    12: "uu",
    13: "u",
    14: "uu",
    15: "u",
    16: "u",
    17: "u",
}


def simd_immediates(sub):
    if sub <= 0x0B or sub in (0x5C, 0x5D):
        return "m"
    if 0x54 <= sub <= 0x5B:
        return "ml"
    if sub == 0x0C:
        return "v"
    if sub == 0x0D:
        return "v"
    if 0x15 <= sub <= 0x22:
        return "l"
    return ""


def atomic_immediates(sub):
    if sub == 0x03:
        return "z"
    return "m"


def copy_immediates(reader, encoder, kinds):
    index = 0
    while index < len(kinds):
        kind = kinds[index]
        if kind == "u":
            encoder.u32(reader.unsigned(32))
        elif kind == "b":
            copy_block_type(reader, encoder)
        elif kind == "m":
            copy_memory_argument(reader, encoder)
        elif kind == "h":
            copy_heap_type(reader, encoder)
        elif kind == "l" or kind == "z":
            encoder.raw(bytes([reader.byte()]))
        elif kind == "v":
            # A v128 constant or a shuffle's sixteen lanes.
            encoder.raw(reader.bytes(16))
        elif kind == "c":
            encoder.raw(bytes([reader.byte()]))
            encoder.u32(reader.unsigned(32))
            copy_heap_type(reader, encoder)
            copy_heap_type(reader, encoder)
        else:
            raise DecodeError(f"unknown immediate kind {kind}")
        index += 1


def copy_instruction(reader, encoder):
    opcode = reader.byte()
    encoder.raw(bytes([opcode]))
    if opcode in PLAIN:
        copy_immediates(reader, encoder, PLAIN[opcode])
        return
    if opcode == 0x0E:
        count = reader.unsigned(32)
        encoder.u32(count)
        for _ in range(count + 1):
            encoder.u32(reader.unsigned(32))
        return
    if opcode == 0x1C:
        count = reader.unsigned(32)
        encoder.u32(count)
        for _ in range(count):
            copy_value_type(reader, encoder)
        return
    if opcode == 0x1F:
        copy_block_type(reader, encoder)
        count = reader.unsigned(32)
        encoder.u32(count)
        for _ in range(count):
            kind = reader.byte()
            encoder.raw(bytes([kind]))
            if kind < 2:
                encoder.u32(reader.unsigned(32))
            encoder.u32(reader.unsigned(32))
        return
    if opcode == 0x41:
        encoder.s32(reader.signed(32))
        return
    if opcode == 0x42:
        encoder.s64(reader.signed(64))
        return
    if opcode == 0x43:
        encoder.raw(reader.bytes(4))
        return
    if opcode == 0x44:
        encoder.raw(reader.bytes(8))
        return
    if opcode in (0xFB, 0xFC, 0xFD, 0xFE):
        sub = reader.unsigned(32)
        encoder.u32(sub)
        if opcode == 0xFB:
            kinds = GC.get(sub)
        elif opcode == 0xFC:
            kinds = MISC.get(sub)
        elif opcode == 0xFD:
            kinds = simd_immediates(sub)
        else:
            kinds = atomic_immediates(sub)
        if kinds is None:
            raise DecodeError(f"unknown sub-opcode {opcode:#x} {sub}")
        copy_immediates(reader, encoder, kinds)
        return
    raise DecodeError(f"unknown opcode {opcode:#x}")


def reencode_body(body, mode):
    reader = Reader(body)
    encoder = Encoder(mode)
    groups = reader.unsigned(32)
    encoder.u32(groups)
    for _ in range(groups):
        encoder.u32(reader.unsigned(32))
        copy_value_type(reader, encoder)
    while reader.position < reader.end:
        copy_instruction(reader, encoder)
    return bytes(encoder.out)


def reencode_module(data, mode):
    if data[:8] != b"\0asm\x01\0\0\0":
        raise DecodeError("not a module")
    reader = Reader(data, 8)
    out = bytearray(data[:8])
    while reader.position < reader.end:
        section_id = reader.byte()
        size = reader.unsigned(32)
        contents = reader.bytes(size)
        if section_id != 10:
            out += bytes([section_id]) + unsigned_leb(size) + contents
            continue
        section = Reader(contents)
        count = section.unsigned(32)
        new_contents = bytearray(unsigned_leb(count))
        for _ in range(count):
            body_size = section.unsigned(32)
            body = reencode_body(section.bytes(body_size), mode)
            new_contents += unsigned_leb(len(body)) + body
        if section.position != section.end:
            raise DecodeError("code section has trailing bytes")
        out += bytes([10]) + unsigned_leb(len(new_contents)) + new_contents
    return bytes(out)


def reencode_tree(source, destination, mode):
    rewritten = 0
    kept = 0
    for directory, _, files in os.walk(source):
        relative = os.path.relpath(directory, source)
        target = os.path.join(destination, relative)
        os.makedirs(target, exist_ok=True)
        for name in files:
            source_path = os.path.join(directory, name)
            target_path = os.path.join(target, name)
            if os.path.lexists(target_path):
                os.remove(target_path)
            if not name.endswith(".wasm"):
                os.symlink(os.path.abspath(source_path), target_path)
                continue
            data = open(source_path, "rb").read()
            try:
                new = reencode_module(data, mode)
                rewritten += 1
            except (DecodeError, IndexError):
                new = data
                kept += 1
            open(target_path, "wb").write(new)
    print(f"{rewritten} modules re-encoded, {kept} kept as they were", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["wide", "memory-index"], required=True)
    parser.add_argument("--tree", action="store_true")
    parser.add_argument("source")
    parser.add_argument("destination")
    arguments = parser.parse_args()
    if arguments.tree:
        reencode_tree(arguments.source, arguments.destination, arguments.mode)
        return
    data = open(arguments.source, "rb").read()
    open(arguments.destination, "wb").write(reencode_module(data, arguments.mode))


if __name__ == "__main__":
    main()
