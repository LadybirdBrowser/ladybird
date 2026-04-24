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


PROPERTY_GROUPS = {
    "#background-properties": [
        # https://drafts.csswg.org/css-backgrounds/#property-index
        "background",
        "background-attachment",
        "background-clip",
        "background-color",
        "background-image",
        "background-origin",
        "background-position",
        "background-position-x",
        "background-position-y",
        "background-repeat",
        "background-size",
    ],
    "#border-properties": [
        # https://drafts.csswg.org/css-backgrounds/#property-index
        "border",
        "border-block-end",
        "border-block-end-color",
        "border-block-end-style",
        "border-block-end-width",
        "border-block-start",
        "border-block-start-color",
        "border-block-start-style",
        "border-block-start-width",
        "border-bottom",
        "border-bottom-color",
        "border-bottom-left-radius",
        "border-bottom-right-radius",
        "border-bottom-style",
        "border-bottom-width",
        "border-color",
        "border-image-outset",
        "border-image-repeat",
        "border-image-slice",
        "border-image-source",
        "border-image-width",
        "border-inline-end",
        "border-inline-end-color",
        "border-inline-end-style",
        "border-inline-end-width",
        "border-inline-start",
        "border-inline-start-color",
        "border-inline-start-style",
        "border-inline-start-width",
        "border-left",
        "border-left-color",
        "border-left-style",
        "border-left-width",
        "border-radius",
        "border-right",
        "border-right-color",
        "border-right-style",
        "border-right-width",
        "border-style",
        "border-top",
        "border-top-color",
        "border-top-left-radius",
        "border-top-right-radius",
        "border-top-style",
        "border-top-width",
        "border-width",
    ],
    "#custom-properties": [
        "custom",
    ],
    "#font-properties": [
        # https://drafts.csswg.org/css-fonts/#property-index
        "font",
        "font-family",
        "font-feature-settings",
        # FIXME: font-kerning
        "font-language-override",
        # FIXME: font-optical-sizing
        # FIXME: font-palette
        "font-size",
        # FIXME: font-size-adjust
        "font-style",
        # FIXME: font-synthesis and longhands
        "font-variant",
        "font-variant-alternates",
        "font-variant-caps",
        "font-variant-east-asian",
        "font-variant-emoji",
        "font-variant-ligatures",
        "font-variant-numeric",
        "font-variant-position",
        "font-variation-settings",
        "font-weight",
        "font-width",
    ],
    "#inline-layout-properties": [
        # https://drafts.csswg.org/css-inline/#property-index
        # FIXME: alignment-baseline
        # FIXME: baseline-shift
        # FIXME: baseline-source
        # FIXME: dominant-baseline
        # FIXME: initial-letter
        # FIXME: initial-letter-align
        # FIXME: initial-letter-wrap
        # FIXME: inline-sizing
        # FIXME: line-edge-fit
        "line-height",
        # FIXME: text-box
        # FIXME: text-box-edge
        # FIXME: text-box-trim
        "vertical-align",
    ],
    "#inline-typesetting-properties": [
        # https://drafts.csswg.org/css-text-4/#property-index
        # FIXME: hanging-punctuation
        # FIXME: hyphenate-character
        # FIXME: hyphenate-limit-chars
        # FIXME: hyphenate-limit-last
        # FIXME: hyphenate-limit-lines
        # FIXME: hyphenate-limit-zone
        # FIXME: hyphens
        "letter-spacing",
        # FIXME: line-break
        # FIXME: line-padding
        "overflow-wrap",
        "tab-size",
        "text-align",
        # FIXME: text-align-all
        # FIXME: text-align-last
        # FIXME: text-autospace
        # FIXME: text-group-align
        "text-indent",
        "text-justify",
        # FIXME: text-spacing
        # FIXME: text-spacing-trim
        "text-transform",
        "text-wrap",
        "text-wrap-mode",
        "text-wrap-style",
        "white-space",
        "white-space-collapse",
        "white-space-trim",
        "word-break",
        # FIXME: word-space-transform
        "word-spacing",
        # FIXME: wrap-after
        # FIXME: wrap-before
        # FIXME: wrap-inside
    ],
    "#margin-properties": [
        "margin",
        "margin-block",
        "margin-block-end",
        "margin-block-start",
        "margin-bottom",
        "margin-inline",
        "margin-inline-end",
        "margin-inline-start",
        "margin-left",
        "margin-right",
        "margin-top",
    ],
    "#padding-properties": [
        "padding",
        "padding-block",
        "padding-block-end",
        "padding-block-start",
        "padding-bottom",
        "padding-inline",
        "padding-inline-end",
        "padding-inline-start",
        "padding-left",
        "padding-right",
        "padding-top",
    ],
    "#text-decoration-properties": [
        "text-decoration",
        "text-decoration-color",
        "text-decoration-line",
        "text-decoration-style",
        "text-decoration-thickness",
    ],
}


def is_alias(pseudo_element: dict) -> bool:
    return "alias-for" in pseudo_element


def write_header_file(out: TextIO, pseudo_elements_data: dict) -> None:
    pseudo_element_count = len(pseudo_elements_data)
    pseudo_element_underlying_type = underlying_type_for_enum(pseudo_element_count)

    out.write(f"""
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Export.h>

namespace Web::CSS {{

enum class PseudoElement : {pseudo_element_underlying_type} {{
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        out.write(f"    {title_casify(name)},\n")

    out.write("""
    KnownPseudoElementCount,

    UnknownWebKit,
};

Optional<PseudoElement> pseudo_element_from_string(StringView);
Optional<PseudoElement> aliased_pseudo_element_from_string(StringView);
WEB_API StringView pseudo_element_name(PseudoElement);

bool is_has_allowed_pseudo_element(PseudoElement);
bool is_tree_abiding_pseudo_element(PseudoElement);
bool is_element_backed_pseudo_element(PseudoElement);
bool is_pseudo_element_root(PseudoElement);
bool pseudo_element_supports_property(PseudoElement, PropertyID);

struct PseudoElementMetadata {
    enum class ParameterType {
        None,
        CompoundSelector,
        IdentList,
        PTNameSelector,
    } parameter_type;
    bool is_valid_as_function;
    bool is_valid_as_identifier;
};
PseudoElementMetadata pseudo_element_metadata(PseudoElement);

}
""")


def write_implementation_file(out: TextIO, pseudo_elements_data: dict) -> None:
    out.write("""
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

Optional<PseudoElement> pseudo_element_from_string(StringView string)
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

Optional<PseudoElement> aliased_pseudo_element_from_string(StringView string)
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

bool pseudo_element_supports_property(PseudoElement pseudo_element, PropertyID property_id)
{
    switch (pseudo_element) {
""")

    for name, pseudo_element in pseudo_elements_data.items():
        if is_alias(pseudo_element):
            continue
        property_whitelist = pseudo_element.get("property-whitelist")
        # No whitelist = accept everything, by falling back to the default case.
        if property_whitelist is None:
            continue

        out.write(f"""
    case PseudoElement::{title_casify(name)}:
        switch (property_id) {{
""")

        for entry in property_whitelist:
            if entry.startswith("FIXME:"):
                continue
            if not entry.startswith("#"):
                out.write(f"        case PropertyID::{title_casify(entry)}:\n")
                continue
            # Categories
            # TODO: Maybe define these in data somewhere too?
            if entry not in PROPERTY_GROUPS:
                print(f"Error: Unrecognized property group name '{entry}' in {name}", file=sys.stderr)
                sys.exit(1)
            for property_name in PROPERTY_GROUPS[entry]:
                out.write(f"        case PropertyID::{title_casify(property_name)}:\n")

        out.write("""
            return true;
        default:
            return false;
        }
""")

    out.write("""
    default:
        return true;
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
