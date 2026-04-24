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

PARAMETER_TYPES = {
    "<an+b>": "ANPlusB",
    "<an+b-of>": "ANPlusBOf",
    "<compound-selector>": "CompoundSelector",
    "<forgiving-selector-list>": "ForgivingSelectorList",
    "<forgiving-relative-selector-list>": "ForgivingRelativeSelectorList",
    "<ident>": "Ident",
    "<language-ranges>": "LanguageRanges",
    "<level>#": "LevelList",
    "<relative-selector-list>": "RelativeSelectorList",
    "<selector-list>": "SelectorList",
}


def write_header_file(out: TextIO, pseudo_classes_data: dict) -> None:
    out.write("""
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>

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

Optional<PseudoClass> pseudo_class_from_string(StringView);
StringView pseudo_class_name(PseudoClass);

struct PseudoClassMetadata {
    enum class ParameterType {
        None,
        ANPlusB,
        ANPlusBOf,
        CompoundSelector,
        ForgivingSelectorList,
        ForgivingRelativeSelectorList,
        Ident,
        LanguageRanges,
        LevelList,
        RelativeSelectorList,
        SelectorList,
    } parameter_type;
    bool is_valid_as_function;
    bool is_valid_as_identifier;
};
PseudoClassMetadata pseudo_class_metadata(PseudoClass);

}
""")


def write_implementation_file(out: TextIO, pseudo_classes_data: dict) -> None:
    out.write("""
#include <LibWeb/CSS/PseudoClass.h>

namespace Web::CSS {

Optional<PseudoClass> pseudo_class_from_string(StringView string)
{
""")

    for name, value in pseudo_classes_data.items():
        alias_for = value.get("legacy-alias-for")
        target_name = title_casify(alias_for) if alias_for is not None else title_casify(name)
        out.write(f"""
    if (string.equals_ignoring_ascii_case("{name}"sv))
        return PseudoClass::{target_name};
""")

    out.write("""

    return {};
}

StringView pseudo_class_name(PseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case PseudoClass::__Count:
        VERIFY_NOT_REACHED();
""")

    for name, value in pseudo_classes_data.items():
        if "legacy-alias-for" in value:
            continue
        out.write(f"""
    case PseudoClass::{title_casify(name)}:
        return "{name}"sv;
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

PseudoClassMetadata pseudo_class_metadata(PseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case PseudoClass::__Count:
        VERIFY_NOT_REACHED();
""")

    for name, value in pseudo_classes_data.items():
        if "legacy-alias-for" in value:
            continue

        argument_string = value["argument"]
        is_valid_as_identifier = argument_string == ""
        is_valid_as_function = argument_string != ""

        if argument_string.endswith("?"):
            is_valid_as_identifier = True
            argument_string = argument_string[:-1]

        parameter_type = "None"
        if is_valid_as_function:
            if argument_string not in PARAMETER_TYPES:
                print(f"Unrecognized pseudo-class argument type: `{argument_string}`", file=sys.stderr)
                sys.exit(1)
            parameter_type = PARAMETER_TYPES[argument_string]

        is_valid_as_function_str = "true" if is_valid_as_function else "false"
        is_valid_as_identifier_str = "true" if is_valid_as_identifier else "false"

        out.write(f"""
    case PseudoClass::{title_casify(name)}:
        return {{
            .parameter_type = PseudoClassMetadata::ParameterType::{parameter_type},
            .is_valid_as_function = {is_valid_as_function_str},
            .is_valid_as_identifier = {is_valid_as_identifier_str},
        }};
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS PseudoClasses", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the PseudoClasses header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the PseudoClasses implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        pseudo_classes_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, pseudo_classes_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, pseudo_classes_data)


if __name__ == "__main__":
    main()
