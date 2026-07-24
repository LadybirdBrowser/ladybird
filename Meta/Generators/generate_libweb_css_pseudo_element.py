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

PARAMETER_TYPES = {
    "<compound-selector>": "CompoundSelector",
    "<ident>+": "IdentList",
    "<pt-name-selector>": "PTNameSelector",
}


def is_alias(pseudo_element: dict) -> bool:
    return "alias-for" in pseudo_element


def write_header_file(out: TextIO, pseudo_elements_data: dict) -> None:
    pseudo_element_count = len(pseudo_elements_data)
    pseudo_element_underlying_type = underlying_type_for_enum(pseudo_element_count)

    synthetic_pseudo_elements = []
    element_reference_pseudo_elements = []
    functional_pseudo_elements = []

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue

        if pseudo_element.get("type") == "function":
            functional_pseudo_elements.append(name)
            continue

        implementation = pseudo_element.get("implementation")

        if implementation == "synthetic":
            synthetic_pseudo_elements.append(name)
        elif implementation == "element-reference":
            element_reference_pseudo_elements.append(name)
        else:
            raise AssertionError(f"Invalid or missing implementation type for pseudo-element `{name}`")

    out.write(f"""
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <LibWeb/Export.h>

namespace Web::CSS {{

enum class PseudoElement : {pseudo_element_underlying_type} {{
""")

    for category in (synthetic_pseudo_elements, element_reference_pseudo_elements, functional_pseudo_elements):
        for name in category:
            out.write(f"    {title_casify(name)},\n")

    out.write(f"""
    KnownPseudoElementCount,

    UnknownWebKit,
}};

constexpr PseudoElement first_synthetic_pseudo_element = PseudoElement::{title_casify(synthetic_pseudo_elements[0])};
constexpr PseudoElement last_synthetic_pseudo_element = PseudoElement::{title_casify(synthetic_pseudo_elements[-1])};
constexpr PseudoElement first_element_reference_pseudo_element = PseudoElement::{title_casify(element_reference_pseudo_elements[0])};
constexpr PseudoElement last_element_reference_pseudo_element = PseudoElement::{title_casify(element_reference_pseudo_elements[-1])};

Optional<PseudoElement> pseudo_element_from_string(Utf16View);
Optional<PseudoElement> aliased_pseudo_element_from_string(Utf16View);
WEB_API StringView pseudo_element_name(PseudoElement);

bool is_has_allowed_pseudo_element(PseudoElement);
bool is_tree_abiding_pseudo_element(PseudoElement);
bool is_element_backed_pseudo_element(PseudoElement);
bool is_pseudo_element_root(PseudoElement);
inline bool is_synthetic_pseudo_element(PseudoElement pseudo_element) {{ return pseudo_element >= first_synthetic_pseudo_element && pseudo_element <= last_synthetic_pseudo_element; }}
inline bool is_element_reference_pseudo_element(PseudoElement pseudo_element) {{ return pseudo_element >= first_element_reference_pseudo_element && pseudo_element <= last_element_reference_pseudo_element; }}

struct PseudoElementMetadata {{
    enum class ParameterType {{
        None,
        CompoundSelector,
        IdentList,
        PTNameSelector,
    }} parameter_type;
    bool is_valid_as_function;
    bool is_valid_as_identifier;
}};
PseudoElementMetadata pseudo_element_metadata(PseudoElement);

}}
""")


def write_implementation_file(out: TextIO, pseudo_elements_data: dict) -> None:
    out.write("""
#include <LibWeb/CSS/PseudoElement.h>

namespace Web::CSS {

Optional<PseudoElement> pseudo_element_from_string(Utf16View string)
{
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        out.write(f"""
    if (string.equals_ignoring_ascii_case("{name}"sv))
        return PseudoElement::{title_casify(name)};
""")

    out.write("""

    return {};
}

Optional<PseudoElement> aliased_pseudo_element_from_string(Utf16View string)
{
""")

    for name, pseudo_element in pseudo_elements_data.items():
        alias_for = pseudo_element.get("alias-for")
        if alias_for is None:
            continue
        out.write(f"""
    if (string.equals_ignoring_ascii_case("{name}"sv))
        return PseudoElement::{title_casify(alias_for)};
""")

    out.write("""

    return {};
}

StringView pseudo_element_name(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        out.write(f"""
    case PseudoElement::{title_casify(name)}:
        return "{name}"sv;
""")

    out.write("""
    case PseudoElement::KnownPseudoElementCount:
    case PseudoElement::UnknownWebKit:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

bool is_has_allowed_pseudo_element(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        if not pseudo_element.get("is-allowed-in-has", False):
            continue
        out.write(f"""
    case PseudoElement::{title_casify(name)}:
        return true;
""")

    out.write("""
    default:
        return false;
    }
}

bool is_tree_abiding_pseudo_element(PseudoElement pseudo_element)
{
    // Element-backed pseudo-elements are always tree-abiding.
    // https://drafts.csswg.org/css-pseudo-4/#element-backed
    if (is_element_backed_pseudo_element(pseudo_element))
        return true;

    switch (pseudo_element) {
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        if not pseudo_element.get("is-tree-abiding", False):
            continue
        out.write(f"""
    case PseudoElement::{title_casify(name)}:
        return true;
""")

    out.write("""
    default:
        return false;
    }
}

bool is_element_backed_pseudo_element(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        if not pseudo_element.get("is-element-backed", False):
            continue
        out.write(f"""
    case PseudoElement::{title_casify(name)}:
        return true;
""")

    out.write("""
    default:
        return false;
    }
}

bool is_pseudo_element_root(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        if not pseudo_element.get("is-pseudo-root", False):
            continue
        out.write(f"""
    case PseudoElement::{title_casify(name)}:
        return true;
""")

    out.write("""
    default:
        return false;
    }
}

PseudoElementMetadata pseudo_element_metadata(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue

        pe_type = pseudo_element.get("type")
        if pe_type == "function":
            is_valid_as_function = True
            is_valid_as_identifier = False
        elif pe_type == "both":
            is_valid_as_function = True
            is_valid_as_identifier = True
        else:
            is_valid_as_function = False
            is_valid_as_identifier = True

        parameter_type = "None"
        if is_valid_as_function:
            function_syntax = pseudo_element["function-syntax"]
            if function_syntax not in PARAMETER_TYPES:
                print(f"Unrecognized pseudo-element parameter type: `{function_syntax}`", file=sys.stderr)
                sys.exit(1)
            parameter_type = PARAMETER_TYPES[function_syntax]
        elif "function-syntax" in pseudo_element:
            print(f"Pseudo-element `::{name}` has `function-syntax` but is not a function type.", file=sys.stderr)
            sys.exit(1)

        is_valid_as_function_str = "true" if is_valid_as_function else "false"
        is_valid_as_identifier_str = "true" if is_valid_as_identifier else "false"

        out.write(f"""
    case PseudoElement::{title_casify(name)}:
        return {{
            .parameter_type = PseudoElementMetadata::ParameterType::{parameter_type},
            .is_valid_as_function = {is_valid_as_function_str},
            .is_valid_as_identifier = {is_valid_as_identifier_str},
        }};
""")

    out.write("""
    case PseudoElement::UnknownWebKit:
        return {
            .parameter_type = PseudoElementMetadata::ParameterType::None,
            .is_valid_as_function = false,
            .is_valid_as_identifier = true,
        };
    case PseudoElement::KnownPseudoElementCount:
        break;
    }
    VERIFY_NOT_REACHED();
}

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS PseudoElement", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the PseudoElement header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the PseudoElement implementation file to generate"
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        pseudo_elements_data = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, pseudo_elements_data)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, pseudo_elements_data)


if __name__ == "__main__":
    main()
