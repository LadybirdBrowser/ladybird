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

from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum


def verify_alphabetical(data: dict, path: str) -> None:
    most_recent_name = ""
    for name in data:
        if name < most_recent_name:
            print(
                f"`{name}` is in the wrong position in `{path}`. Please keep this list alphabetical!", file=sys.stderr
            )
            sys.exit(1)
        most_recent_name = name


def write_header_file(out: TextIO, environment_variables: dict) -> None:
    underlying_type = underlying_type_for_enum(len(environment_variables))
    out.write(f"""
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS {{

enum class EnvironmentVariable : {underlying_type} {{
""")

    for name in environment_variables:
        out.write(f"    {title_casify(name)},\n")

    out.write("""
};

Optional<EnvironmentVariable> environment_variable_from_string(StringView);
StringView to_string(EnvironmentVariable);

ValueType environment_variable_type(EnvironmentVariable);
u32 environment_variable_dimension_count(EnvironmentVariable);
}
""")


def write_implementation_file(out: TextIO, environment_variables: dict) -> None:
    out.write("""
#include <LibWeb/CSS/EnvironmentVariable.h>

namespace Web::CSS {

Optional<EnvironmentVariable> environment_variable_from_string(StringView string)
{
""")
    for name in environment_variables:
        out.write(f"""
    if (string.equals_ignoring_ascii_case("{name}"sv))
        return EnvironmentVariable::{title_casify(name)};
""")

    out.write("""

    return {};
}

StringView to_string(EnvironmentVariable environment_variable)
{
    switch (environment_variable) {
""")

    for name in environment_variables:
        out.write(f"""
    case EnvironmentVariable::{title_casify(name)}:
        return "{name}"sv;
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

ValueType environment_variable_type(EnvironmentVariable environment_variable)
{
    switch (environment_variable) {
""")

    for name, variable in environment_variables.items():
        value_type = variable["type"].strip("<>")
        out.write(f"""
    case EnvironmentVariable::{title_casify(name)}:
        return ValueType::{title_casify(value_type)};
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

u32 environment_variable_dimension_count(EnvironmentVariable environment_variable)
{
    switch (environment_variable) {
""")

    for name, variable in environment_variables.items():
        out.write(f"""
    case EnvironmentVariable::{title_casify(name)}:
        return {variable["dimensions"]};
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS EnvironmentVariable", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the EnvironmentVariable header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the EnvironmentVariable implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        environment_variables = json.load(input_file)

    verify_alphabetical(environment_variables, args.json)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, environment_variables)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, environment_variables)


if __name__ == "__main__":
    main()
