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


def write_header_file(out: TextIO, pseudo_classes_data: dict) -> None:
    out.write("""
#pragma once

namespace Web::CSS {

enum class PseudoClass {
""")

    for name, value in pseudo_classes_data.items():
        if "legacy-alias-for" in value:
            continue
        out.write(f"    {title_casify(name)},\n")

    out.write("""
    __Count,
};

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS PseudoClasses", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the PseudoClasses header file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        pseudo_classes_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, pseudo_classes_data)


if __name__ == "__main__":
    main()
