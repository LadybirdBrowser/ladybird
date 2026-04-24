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


PARAMETER_TYPE_NAMES = {
    "angle": "Angle",
    "length": "Length",
    "length-none": "LengthNone",
    "length-percentage": "LengthPercentage",
    "number": "Number",
    "number-percentage": "NumberPercentage",
}


def title_casify_transform_function(name: str) -> str:
    # Transform function names look like `fooBar`, so we just have to make the first character uppercase.
    return name[0].upper() + name[1:]


def write_header_file(out: TextIO, transforms_data: dict) -> None:
    out.write("""
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Web::CSS {

""")

    out.write("enum class TransformFunction {\n")
    for name in transforms_data:
        out.write(f"    {title_casify_transform_function(name)},\n")
    out.write("};\n")

    out.write("Optional<TransformFunction> transform_function_from_string(StringView);\n")
    out.write("StringView to_string(TransformFunction);\n")

    out.write("""
enum class TransformFunctionParameterType {
    Angle,
    Length,
    LengthNone,
    LengthPercentage,
    Number,
    NumberPercentage
};

struct TransformFunctionParameter {
    TransformFunctionParameterType type;
    bool required;
};

struct TransformFunctionMetadata {
    Vector<TransformFunctionParameter> parameters;
};
TransformFunctionMetadata transform_function_metadata(TransformFunction);

}
""")


def write_implementation_file(out: TextIO, transforms_data: dict) -> None:
    out.write("""
#include <LibWeb/CSS/TransformFunctions.h>
#include <AK/Assertions.h>

namespace Web::CSS {

Optional<TransformFunction> transform_function_from_string(StringView name)
{
""")
    for name in transforms_data:
        out.write(f"""
    if (name.equals_ignoring_ascii_case("{name}"sv))
        return TransformFunction::{title_casify_transform_function(name)};
""")

    out.write("""
    return {};
}

StringView to_string(TransformFunction transform_function)
{
    switch (transform_function) {
""")
    for name in transforms_data:
        out.write(f"""
    case TransformFunction::{title_casify_transform_function(name)}:
        return "{name}"sv;
""")

    out.write("""
    default:
        VERIFY_NOT_REACHED();
    }
}

TransformFunctionMetadata transform_function_metadata(TransformFunction transform_function)
{
    switch (transform_function) {
""")
    for name, value in transforms_data.items():
        out.write(f"""
    case TransformFunction::{title_casify_transform_function(name)}:
        return TransformFunctionMetadata {{
            .parameters = {{""")

        parameters = value["parameters"]
        for index, parameter in enumerate(parameters):
            parameter_type_name = parameter["type"].strip("<>")
            if parameter_type_name not in PARAMETER_TYPE_NAMES:
                raise ValueError(f"Unknown transform function parameter type: {parameter_type_name}")
            parameter_type = PARAMETER_TYPE_NAMES[parameter_type_name]

            separator = " " if index == 0 else ", "
            required = "true" if parameter["required"] else "false"
            out.write(f"{separator}{{ TransformFunctionParameterType::{parameter_type}, {required}}}")

        out.write(" }\n    };\n")

    out.write("""
    default:
        VERIFY_NOT_REACHED();
    }
}

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS TransformFunctions", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the TransformFunctions header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the TransformFunctions implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        transforms_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, transforms_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, transforms_data)


if __name__ == "__main__":
    main()
