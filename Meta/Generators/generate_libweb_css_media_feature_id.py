#!/usr/bin/env python3

# Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum


def write_header_file(out: TextIO, media_feature_data: dict) -> None:
    underlying_type = underlying_type_for_enum(len(media_feature_data))
    out.write(f"""#pragma once

#include <AK/Types.h>

namespace Web::CSS {{

enum class MediaFeatureID : {underlying_type} {{""")

    out.writelines(
        f"""
    {title_casify(name)},"""
        for name in media_feature_data
    )

    out.write("""
};

inline constexpr size_t media_feature_count = """)
    out.write(str(len(media_feature_data)))
    out.write(""";

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS MediaFeatureID", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the MediaFeatureID header file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        media_feature_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, media_feature_data)


if __name__ == "__main__":
    main()
