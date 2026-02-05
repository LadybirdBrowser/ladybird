#!/usr/bin/env python3

# Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
# Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json

from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Any


class GenerateAccessor(Enum):
    NO = False
    YES = True


class GenerateInverseAccessor(Enum):
    NO = False
    YES = True


@dataclass
class LookupTable:
    first_pointer: int
    max_code_point: int
    code_points: list[int]
    generate_accessor: GenerateAccessor
    generate_inverse_accessor: GenerateInverseAccessor


@dataclass
class LookupTables:
    gb18030_ranges: list[Any]
    indexes: dict[str, LookupTable]


def prepare_table(
    data: list[Any],
    generate_accessor: GenerateAccessor = GenerateAccessor.NO,
) -> LookupTable:
    code_points = []
    max_code_point = 0
    first_pointer = 0

    for entry in data:
        if entry is None:
            if not code_points:
                first_pointer += 1
            else:
                code_points.append(0xFFFD)
                max_code_point = max(max_code_point, code_points[-1])
        else:
            code_points.append(int(entry))
            max_code_point = max(max_code_point, code_points[-1])

    if generate_accessor == GenerateAccessor.YES:
        while code_points and code_points[-1] == 0xFFFD:
            code_points.pop()
    else:
        assert first_pointer == 0

    return LookupTable(
        first_pointer=first_pointer,
        max_code_point=max_code_point,
        code_points=code_points,
        generate_accessor=GenerateAccessor.YES,
        generate_inverse_accessor=GenerateInverseAccessor.YES,
    )


def generate_table(name: str, table: LookupTable) -> str:
    max_u16 = (1 << 16) - 1
    value_type = "u32" if table.max_code_point > max_u16 else "u16"
    size = len(table.code_points)

    lines = []

    if table.first_pointer > 0:
        lines.append(f"static constexpr u32 s_{name}_index_first_pointer = {table.first_pointer};")

    lines.append(f"static constexpr Array<{value_type}, {size}> s_{name}_index {{")

    formatted_points = []
    for i, point in enumerate(table.code_points):
        formatted_points.append(f"0x{point:04x}")
        if i != len(table.code_points) - 1:
            if i % 16 == 15:
                formatted_points.append(",\n    ")
            else:
                formatted_points.append(", ")

    lines.append(f"    {' '.join(formatted_points)}")
    lines.append("};")

    if table.generate_accessor:
        lines.append(f"Optional<u32> index_{name}_code_point(u32 pointer);")

    if table.generate_inverse_accessor:
        lines.append(f"Optional<u32> code_point_{name}_index(u32 code_point);")

    return "\n".join(lines)


def generate_header_file(tables: LookupTables, output_path: Path) -> None:
    gb18030_ranges_size = len(tables.gb18030_ranges)

    content = f"""#pragma once

#include <AK/Array.h>
#include <AK/Types.h>

namespace TextCodec {{

struct Gb18030RangeEntry {{
    u32 pointer;
    u32 code_point;
}};

static constexpr Array<Gb18030RangeEntry, {gb18030_ranges_size}> s_gb18030_ranges {{ {{
"""

    for range_entry in tables.gb18030_ranges:
        pointer = range_entry[0]
        code_point = range_entry[1]
        content += f"    {{ {pointer}, 0x{code_point:04x} }},\n"

    content += "} };\n\n"

    for name, table in tables.indexes.items():
        content += generate_table(name, table) + "\n\n"

    content += "}\n"

    with open(output_path, "w") as f:
        f.write(content)


def generate_table_accessor(name: str, table: LookupTable) -> str:
    if table.first_pointer > 0:
        return f"""
Optional<u32> index_{name}_code_point(u32 pointer)
{{
    if (pointer < s_{name}_index_first_pointer || pointer - s_{name}_index_first_pointer >= s_{name}_index.size())
        return {{}};
    auto value = s_{name}_index[pointer - s_{name}_index_first_pointer];
    if (value == 0xfffd)
        return {{}};
    return value;
}}
"""
    else:
        return f"""
Optional<u32> index_{name}_code_point(u32 pointer)
{{
    if (pointer >= s_{name}_index.size())
        return {{}};
    auto value = s_{name}_index[pointer];
    if (value == 0xfffd)
        return {{}};
    return value;
}}
"""


def generate_inverse_table_accessor(name: str, table: LookupTable) -> str:
    if table.first_pointer > 0:
        return f"""
Optional<u32> code_point_{name}_index(u32 code_point)
{{
    for (u32 i = 0; i < s_{name}_index.size(); ++i) {{
        if (s_{name}_index[i] == code_point) {{
            return s_{name}_index_first_pointer + i;
        }}
    }}
    return {{}};
}}
"""
    else:
        return f"""
Optional<u32> code_point_{name}_index(u32 code_point)
{{
    for (u32 i = 0; i < s_{name}_index.size(); ++i) {{
        if (s_{name}_index[i] == code_point) {{
            return i;
        }}
    }}
    return {{}};
}}
"""


def generate_implementation_file(tables: LookupTables, output_path: Path) -> None:
    content = """
#include <LibTextCodec/LookupTables.h>

namespace TextCodec {
"""

    for name, table in tables.indexes.items():
        if table.generate_accessor:
            content += generate_table_accessor(name, table)
        if table.generate_inverse_accessor:
            content += generate_inverse_table_accessor(name, table)

    content += "\n}\n"

    with open(output_path, "w") as f:
        f.write(content)


def main():
    parser = argparse.ArgumentParser(description="Generate text codec lookup tables", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument(
        "-h", "--generated-header-path", required=True, help="Path to the lookup table header file to generate"
    )
    parser.add_argument(
        "-c",
        "--generated-implementation-path",
        required=True,
        help="Path to the lookup table implementation file to generate",
    )
    parser.add_argument("-j", "--json-path", required=True, help="Path to the JSON file to read from")

    args = parser.parse_args()

    with open(args.json_path, "r") as f:
        data = json.load(f)

    gb18030_table = prepare_table(data["gb18030"], GenerateAccessor.YES)

    # FIXME: Update JSON to match GB-18030-2022 Encoding specification (https://github.com/whatwg/encoding/issues/312)
    # NOTE: See https://commits.webkit.org/264918@main
    gb18030_updates = {
        7182: 0xFE10,
        7183: 0xFE12,
        7184: 0xFE11,
        7185: 0xFE13,
        7186: 0xFE14,
        7187: 0xFE15,
        7188: 0xFE16,
        7201: 0xFE17,
        7202: 0xFE18,
        7208: 0xFE19,
        23775: 0x9FB4,
        23783: 0x9FB5,
        23788: 0x9FB6,
        23789: 0x9FB7,
        23795: 0x9FB8,
        23812: 0x9FB9,
        23829: 0x9FBA,
        23845: 0x9FBB,
    }

    for index, value in gb18030_updates.items():
        if index < len(gb18030_table.code_points):
            gb18030_table.code_points[index] = value

    tables = LookupTables(
        gb18030_ranges=data["gb18030-ranges"],
        indexes={
            "gb18030": gb18030_table,
            "big5": prepare_table(data["big5"], GenerateAccessor.YES),
            "jis0208": prepare_table(data["jis0208"], GenerateAccessor.YES),
            "jis0212": prepare_table(data["jis0212"], GenerateAccessor.YES),
            "euc_kr": prepare_table(data["euc-kr"], GenerateAccessor.YES),
            "ibm866": prepare_table(data["ibm866"]),
            "iso_2022_jp_katakana": prepare_table(data["iso-2022-jp-katakana"], GenerateAccessor.YES),
            "iso_8859_2": prepare_table(data["iso-8859-2"]),
            "iso_8859_3": prepare_table(data["iso-8859-3"]),
            "iso_8859_4": prepare_table(data["iso-8859-4"]),
            "iso_8859_5": prepare_table(data["iso-8859-5"]),
            "iso_8859_6": prepare_table(data["iso-8859-6"]),
            "iso_8859_7": prepare_table(data["iso-8859-7"]),
            "iso_8859_8": prepare_table(data["iso-8859-8"]),
            "iso_8859_10": prepare_table(data["iso-8859-10"]),
            "iso_8859_13": prepare_table(data["iso-8859-13"]),
            "iso_8859_14": prepare_table(data["iso-8859-14"]),
            "iso_8859_15": prepare_table(data["iso-8859-15"]),
            "iso_8859_16": prepare_table(data["iso-8859-16"]),
            "koi8_r": prepare_table(data["koi8-r"]),
            "koi8_u": prepare_table(data["koi8-u"]),
            "macintosh": prepare_table(data["macintosh"]),
            "windows_874": prepare_table(data["windows-874"]),
            "windows_1250": prepare_table(data["windows-1250"]),
            "windows_1251": prepare_table(data["windows-1251"]),
            "windows_1252": prepare_table(data["windows-1252"]),
            "windows_1253": prepare_table(data["windows-1253"]),
            "windows_1254": prepare_table(data["windows-1254"]),
            "windows_1255": prepare_table(data["windows-1255"]),
            "windows_1256": prepare_table(data["windows-1256"]),
            "windows_1257": prepare_table(data["windows-1257"]),
            "windows_1258": prepare_table(data["windows-1258"]),
            "x_mac_cyrillic": prepare_table(data["x-mac-cyrillic"]),
        },
    )

    generate_header_file(tables, Path(args.generated_header_path))
    generate_implementation_file(tables, Path(args.generated_implementation_path))


if __name__ == "__main__":
    main()
