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

from Utils.utils import snake_casify
from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum


def json_is_valid(dimensions_data: dict, json_path: str) -> bool:
    is_valid = True
    most_recent_dimension_name = ""
    for dimension_name, units in dimensions_data.items():
        # Dimensions should be in alphabetical order
        if dimension_name.lower() < most_recent_dimension_name.lower():
            print(
                f"{json_path}: Dimension `{dimension_name}` is in the wrong position. "
                "Please keep this list alphabetical!",
                file=sys.stderr,
            )
            is_valid = False
        most_recent_dimension_name = dimension_name

        most_recent_unit_name = ""
        canonical_unit = None
        for unit_name, unit in units.items():
            # Units should be in alphabetical order
            if unit_name.lower() < most_recent_unit_name.lower():
                print(
                    f"{json_path}: {dimension_name} unit `{unit_name}` is in the wrong position. "
                    "Please keep this list alphabetical!",
                    file=sys.stderr,
                )
                is_valid = False
            most_recent_unit_name = unit_name

            is_canonical_unit = unit.get("is-canonical-unit") is True
            number_of_canonical_unit = unit.get("number-of-canonical-unit")
            relative_to = unit.get("relative-to")
            provided_count = (
                (1 if is_canonical_unit else 0)
                + (1 if number_of_canonical_unit is not None else 0)
                + (1 if relative_to is not None else 0)
            )
            if provided_count != 1:
                print(
                    f"{json_path}: {dimension_name} unit `{unit_name}` must have exactly 1 of "
                    "`is-canonical-unit: true`, `number-of-canonical-unit`, or `relative-to` provided.",
                    file=sys.stderr,
                )
                is_valid = False
            if is_canonical_unit:
                if canonical_unit is not None:
                    print(
                        f"{json_path}: {dimension_name} unit `{unit_name}` marked canonical, "
                        f"but `{canonical_unit}` was already. Must have exactly 1.",
                        file=sys.stderr,
                    )
                    is_valid = False
                else:
                    canonical_unit = unit_name
            if relative_to is not None:
                if dimension_name == "length":
                    if relative_to not in ("font", "viewport"):
                        print(
                            f"{json_path}: {dimension_name} unit `{unit_name}` is marked as relative to "
                            f"`{relative_to}`, which is unsupported.",
                            file=sys.stderr,
                        )
                        is_valid = False
                else:
                    print(
                        f"{json_path}: {dimension_name} unit `{unit_name}` is marked as relative, "
                        "but only relative length units are currently supported.",
                        file=sys.stderr,
                    )
                    is_valid = False

        if canonical_unit is None:
            print(
                f"{json_path}: {dimension_name} has no unit marked as canonical. Must have exactly 1.",
                file=sys.stderr,
            )
            is_valid = False

    return is_valid


def canonical_unit_name(units: dict) -> str:
    for unit_name, unit in units.items():
        if unit.get("is-canonical-unit") is True:
            return unit_name
    raise ValueError("No canonical unit found")


def write_header_file(out: TextIO, dimensions_data: dict) -> None:
    out.write("""
#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>

namespace Web::CSS {
""")

    enum_type = underlying_type_for_enum(len(dimensions_data))
    out.write(f"enum class DimensionType : {enum_type} {{\n")
    for name in dimensions_data:
        out.write(f"    {title_casify(name)},\n")
    out.write("""
};

Optional<DimensionType> dimension_for_unit(StringView);
""")

    for dimension_name, units in dimensions_data.items():
        name_titlecase = title_casify(dimension_name)
        name_snakecase = snake_casify(dimension_name)
        unit_enum_type = underlying_type_for_enum(len(units))
        canonical = title_casify(canonical_unit_name(units))

        out.write(f"""
enum class {name_titlecase}Unit : {unit_enum_type} {{
""")
        for unit_name in units:
            out.write(f"    {title_casify(unit_name)},\n")
        out.write(f"""
}};
constexpr {name_titlecase}Unit canonical_{name_snakecase}_unit() {{ return {name_titlecase}Unit::{canonical}; }}
Optional<{name_titlecase}Unit> string_to_{name_snakecase}_unit(StringView);
FlyString to_string({name_titlecase}Unit);
bool units_are_compatible({name_titlecase}Unit, {name_titlecase}Unit);
double ratio_between_units({name_titlecase}Unit, {name_titlecase}Unit);
""")

    out.write("""
bool is_absolute(LengthUnit);
bool is_font_relative(LengthUnit);
bool is_viewport_relative(LengthUnit);
inline bool is_relative(LengthUnit unit) { return !is_absolute(unit); }

}
""")


def format_ratio(value) -> str:
    # JSON ints render as ints, floats as floats (matching String::number output).
    return str(value)


def write_implementation_file(out: TextIO, dimensions_data: dict) -> None:
    out.write("""
#include <LibWeb/CSS/Units.h>

namespace Web::CSS {

Optional<DimensionType> dimension_for_unit(StringView unit_name)
{
""")

    for dimension_name, units in dimensions_data.items():
        name_titlecase = title_casify(dimension_name)
        out.write("    if (")
        first = True
        for unit_name in units:
            if first:
                first = False
            else:
                out.write("\n         || ")
            out.write(f'unit_name.equals_ignoring_ascii_case("{unit_name}"sv)')
        out.write(f""")
        return DimensionType::{name_titlecase};
""")

    out.write("""
    return {};
}
""")

    for dimension_name, units in dimensions_data.items():
        name_titlecase = title_casify(dimension_name)
        name_snakecase = snake_casify(dimension_name)
        canonical = title_casify(canonical_unit_name(units))

        out.write(f"""
Optional<{name_titlecase}Unit> string_to_{name_snakecase}_unit(StringView unit_name)
{{
""")
        for unit_name in units:
            out.write(f"""
    if (unit_name.equals_ignoring_ascii_case("{unit_name}"sv))
        return {name_titlecase}Unit::{title_casify(unit_name)};""")

        out.write(f"""
    return {{}};
}}

FlyString to_string({name_titlecase}Unit value)
{{
    switch (value) {{""")

        for unit_name in units:
            out.write(f"""
    case {name_titlecase}Unit::{title_casify(unit_name)}:
        return "{unit_name}"_fly_string;""")

        out.write(f"""
    default:
        VERIFY_NOT_REACHED();
    }}
}}

bool units_are_compatible({name_titlecase}Unit a, {name_titlecase}Unit b)
{{
    auto is_absolute = []({name_titlecase}Unit unit) -> bool {{
        switch (unit) {{
""")
        # https://drafts.csswg.org/css-values-4/#compatible-units
        for unit_name, unit in units.items():
            if "relative-to" in unit:
                continue
            out.write(f"        case {name_titlecase}Unit::{title_casify(unit_name)}:\n")

        out.write(f"""
            return true;
        default:
            return false;
        }}
    }};

    return is_absolute(a) && is_absolute(b);
}}

double ratio_between_units({name_titlecase}Unit from, {name_titlecase}Unit to)
{{
    if (from == to)
        return 1;

    auto ratio_to_canonical_unit = []({name_titlecase}Unit unit) -> double {{
        switch (unit) {{
""")
        for unit_name, unit in units.items():
            if "relative-to" in unit:
                continue
            ratio = unit.get("number-of-canonical-unit")
            if ratio is not None:
                ratio_str = format_ratio(ratio)
            else:
                # This must be the canonical unit, so the ratio is 1.
                ratio_str = "1"
            out.write(f"""
        case {name_titlecase}Unit::{title_casify(unit_name)}:
            return {ratio_str};
""")

        out.write(f"""
        default:
            // `from` is a relative unit, so this isn't valid.
            VERIFY_NOT_REACHED();
        }}
    }};

    if (to == {name_titlecase}Unit::{canonical})
        return ratio_to_canonical_unit(from);
    return ratio_to_canonical_unit(from) / ratio_to_canonical_unit(to);
}}
""")

    # Length-specific functions
    length_units = dimensions_data["length"]

    out.write("""
bool is_absolute(LengthUnit unit)
{
    switch (unit) {
""")
    for unit_name, unit in length_units.items():
        if "relative-to" in unit:
            continue
        out.write(f"    case LengthUnit::{title_casify(unit_name)}:\n")
    out.write("""
        return true;
    default:
        return false;
    }
}

bool is_font_relative(LengthUnit unit)
{
    switch (unit) {
""")
    for unit_name, unit in length_units.items():
        if unit.get("relative-to") != "font":
            continue
        out.write(f"    case LengthUnit::{title_casify(unit_name)}:\n")
    out.write("""
        return true;
    default:
        return false;
    }
}

bool is_viewport_relative(LengthUnit unit)
{
    switch (unit) {
""")
    for unit_name, unit in length_units.items():
        if unit.get("relative-to") != "viewport":
            continue
        out.write(f"    case LengthUnit::{title_casify(unit_name)}:\n")
    out.write("""
        return true;
    default:
        return false;
    }
}

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS Units", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the Units header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the Units implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        dimensions_data = json.load(input_file)

    if not json_is_valid(dimensions_data, args.json):
        sys.exit(1)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, dimensions_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, dimensions_data)


if __name__ == "__main__":
    main()
