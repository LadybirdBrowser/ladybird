#!/usr/bin/env python3

# Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.utils import make_name_acceptable_cpp
from Utils.utils import snake_casify


def write_header_file(out: TextIO, units_data: dict) -> None:
    out.write("""
#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

// https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory
namespace Web::CSS {

GC::Ref<CSSUnitValue> number(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> percent(JS::VM&, WebIDL::Double value);
""")

    for dimension_name, units in units_data.items():
        dimension_acceptable_cpp = make_name_acceptable_cpp(snake_casify(dimension_name, trim_leading_underscores=True))
        out.write(f"\n// <{dimension_acceptable_cpp}>\n")
        for unit_name in units:
            unit_acceptable_cpp = make_name_acceptable_cpp(unit_name.lower())
            out.write(f"GC::Ref<CSSUnitValue> {unit_acceptable_cpp}(JS::VM&, WebIDL::Double value);\n")

    out.write("""
}
""")


def write_implementation_file(out: TextIO, units_data: dict) -> None:
    out.write("""
#include <AK/FlyString.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/GeneratedCSSNumericFactoryMethods.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory
inline GC::Ref<CSSUnitValue> numeric_factory(JS::VM& vm, WebIDL::Double value, FlyString unit)
{
    // All of the above methods must, when called with a double value, return a new CSSUnitValue whose value internal
    // slot is set to value and whose unit internal slot is set to the name of the method as defined here.
    return CSSUnitValue::create(*vm.current_realm(), value, move(unit));
}

GC::Ref<CSSUnitValue> number(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "number"_fly_string);
}

GC::Ref<CSSUnitValue> percent(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "percent"_fly_string);
}

""")

    for dimension_name, units in units_data.items():
        dimension_acceptable_cpp = make_name_acceptable_cpp(snake_casify(dimension_name, trim_leading_underscores=True))
        out.write(f"\n// <{dimension_acceptable_cpp}>\n")
        for unit_name in units:
            unit_acceptable_cpp = make_name_acceptable_cpp(unit_name.lower())
            out.write(f"""
GC::Ref<CSSUnitValue> {unit_acceptable_cpp}(JS::VM& vm, WebIDL::Double value)
{{
    return numeric_factory(vm, value, "{unit_name}"_fly_string);
}}
""")

    out.write("""
}
""")


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
    parser.add_argument(
        "-h", "--header", required=True, help="Path to the CSSNumericFactoryMethods header file to generate"
    )
    parser.add_argument(
        "-c",
        "--implementation",
        required=True,
        help="Path to the CSSNumericFactoryMethods implementation file to generate",
    )
    parser.add_argument("-i", "--idl", required=True, help="Path to the CSSNumericFactoryMethods IDL file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        units_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, units_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, units_data)

    with open(args.idl, "w", encoding="utf-8") as output_file:
        write_idl_file(output_file, units_data)


if __name__ == "__main__":
    main()
