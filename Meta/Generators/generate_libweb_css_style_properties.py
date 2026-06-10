#!/usr/bin/env python3

# Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
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


def css_property_to_idl_attribute(property_name: str, lowercase_first: bool = False) -> str:
    # https://drafts.csswg.org/cssom/#css-property-to-idl-attribute
    actual_property_name = property_name[1:] if lowercase_first else property_name
    output = []
    uppercase_next = False
    for c in actual_property_name:
        if c == "-":
            uppercase_next = True
        elif uppercase_next:
            uppercase_next = False
            output.append(c.upper())
        else:
            output.append(c)
    return "".join(output)


def write_header_file(out: TextIO, properties: dict) -> None:
    out.write("""
#pragma once

#include <AK/String.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#define ENUMERATE_GENERATED_CSS_STYLE_PROPERTIES \\
""")

    for name in properties:
        name_acceptable_cpp = make_name_acceptable_cpp(snake_casify(name, trim_leading_underscores=True))
        out.write(f"""    WebIDL::ExceptionOr<void> set_{name_acceptable_cpp}(StringView value); \\
    String {name_acceptable_cpp}() const; \\
""")

    out.write("""
""")


def write_implementation_file(out: TextIO, properties: dict) -> None:
    out.write("""
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {
""")

    for name in properties:
        name_acceptable_cpp = make_name_acceptable_cpp(snake_casify(name, trim_leading_underscores=True))
        out.write(f"""
WebIDL::ExceptionOr<void> CSSStyleProperties::set_{name_acceptable_cpp}(StringView value)
{{
    return set_property("{name}"_utf16_fly_string, value, ""sv);
}}

String CSSStyleProperties::{name_acceptable_cpp}() const
{{
    return get_property_value("{name}"_utf16_fly_string);
}}
""")

    out.write("""
} // namespace Web::CSS
""")


def write_idl_file(out: TextIO, properties: dict) -> None:
    out.write("""
interface mixin GeneratedCSSStyleProperties {
""")

    for name in properties:
        snake_case_name = snake_casify(name, trim_leading_underscores=True)
        name_acceptable_cpp = make_name_acceptable_cpp(snake_case_name)

        # https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-camel-cased-attribute
        name_camelcase = css_property_to_idl_attribute(name)
        out.write(f"""
    [CEReactions, LegacyNullToEmptyString, AttributeCallbackName={snake_case_name}_regular, ImplementedAs={name_acceptable_cpp}] attribute CSSOMString {name_camelcase};
""")

        # For each CSS property property that is a supported CSS property and that begins with the string -webkit-,
        # the following partial interface applies where webkit-cased attribute is obtained by running the CSS property
        # to IDL attribute algorithm for property, with the lowercase first flag set.
        if name.startswith("-webkit-"):
            name_webkit = css_property_to_idl_attribute(name, lowercase_first=True)
            out.write(f"""
    [CEReactions, LegacyNullToEmptyString, AttributeCallbackName={snake_case_name}_webkit, ImplementedAs={name_acceptable_cpp}] attribute CSSOMString {name_webkit};
""")

        # For each CSS property property that is a supported CSS property, except for properties that have no
        # "-" (U+002D) in the property name, the following partial interface applies where dashed attribute is
        # property.
        if "-" in name:
            out.write(f"""
    [CEReactions, LegacyNullToEmptyString, AttributeCallbackName={snake_case_name}_dashed, ImplementedAs={name_acceptable_cpp}] attribute CSSOMString {name};
""")

    out.write("""
};
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS StyleProperties", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the CSSStyleProperties header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the CSSStyleProperties implementation file to generate"
    )
    parser.add_argument("-i", "--idl", required=True, help="Path to the CSSStyleProperties IDL file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        properties = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, properties)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, properties)

    with open(args.idl, "w", encoding="utf-8") as output_file:
        write_idl_file(output_file, properties)


if __name__ == "__main__":
    main()
