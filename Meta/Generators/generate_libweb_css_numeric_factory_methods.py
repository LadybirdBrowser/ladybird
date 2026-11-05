#!/usr/bin/env python3

# Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.css_dimensions import get_css_dimensions
from Utils.css_dimensions import load_css_dimensions
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import snake_casify


def write_idl_file(out: TextIO, units_data: dict) -> None:
    out.write("""
partial namespace CSS {
    CSSUnitValue number(double value);
    CSSUnitValue percent(double value);

""")

    for dimension_name, units in units_data.items():
        dimension_acceptable_cpp = make_name_acceptable_cpp(snake_casify(dimension_name, trim_leading_underscores=True))
        out.write(f"""
    // <{dimension_acceptable_cpp}>
""")
        for unit_name in units:
            unit_acceptable_cpp = make_name_acceptable_cpp(unit_name.lower())
            out.write(f"    [ImplementedAs={unit_acceptable_cpp}] CSSUnitValue {unit_name}(double value);\n")

    out.write("""
};
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS NumericFactoryMethods", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-i", "--idl", required=True, help="Path to the CSSNumericFactoryMethods IDL file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    load_css_dimensions(args.json)
    units_data = get_css_dimensions()

    with open(args.idl, "w", encoding="utf-8") as output_file:
        write_idl_file(output_file, units_data)


if __name__ == "__main__":
    main()
