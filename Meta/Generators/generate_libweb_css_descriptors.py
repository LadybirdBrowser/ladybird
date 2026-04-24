#!/usr/bin/env python3

# Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
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

from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum


def is_legacy_alias(descriptor: dict) -> bool:
    return "legacy-alias-for" in descriptor


def collect_all_descriptors(at_rules_data: dict) -> list:
    names = set()
    for at_rule in at_rules_data.values():
        for descriptor_name, descriptor in at_rule.get("descriptors", {}).items():
            if is_legacy_alias(descriptor):
                continue
            names.add(descriptor_name)
    return sorted(names)


def write_header_file(out: TextIO, at_rules_data: dict, all_descriptors: list) -> None:
    at_rule_count = len(at_rules_data)
    at_rule_id_underlying_type = underlying_type_for_enum(at_rule_count)
    descriptor_id_underlying_type = underlying_type_for_enum(len(all_descriptors))

    out.write(f"""
#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {{

enum class AtRuleID : {at_rule_id_underlying_type} {{
""")

    for at_rule_name in at_rules_data:
        out.write(f"    {title_casify(at_rule_name)},\n")

    out.write(f"""
}};

FlyString to_string(AtRuleID);

enum class DescriptorID : {descriptor_id_underlying_type} {{
""")
    for descriptor_name in all_descriptors:
        out.write(f"    {title_casify(descriptor_name)},\n")

    out.write("""
    Custom,
};

Optional<DescriptorID> descriptor_id_from_string(AtRuleID, StringView);
FlyString to_string(DescriptorID);

bool at_rule_supports_descriptor(AtRuleID, DescriptorID);
RefPtr<StyleValue const> descriptor_initial_value(AtRuleID, DescriptorID);

struct DescriptorMetadata {
    enum class ValueType {
        // FIXME: Parse the grammar instead of hard-coding all the options!
        CounterStyleSystem,
        CounterStyleAdditiveSymbols,
        CounterStyleName,
        CounterStyleNegative,
        CounterStylePad,
        CounterStyleRange,
        CropOrCross,
        FamilyName,
        FontSrcList,
        FontWeightAbsolutePair,
        Length,
        OptionalDeclarationValue,
        PageSize,
        PositivePercentage,
        String,
        Symbol,
        Symbols,
        UnicodeRangeTokens,
    };
    Vector<Variant<Keyword, PropertyID, ValueType>> syntax;
    bool allow_arbitrary_substitution_functions { false };
};

DescriptorMetadata get_descriptor_metadata(AtRuleID, DescriptorID);

}
""")


VALUE_TYPE_NAMES = {
    "<family-name>": "FamilyName",
    "<font-src-list>": "FontSrcList",
    "<font-weight-absolute>{1,2}": "FontWeightAbsolutePair",
    "<declaration-value>?": "OptionalDeclarationValue",
    "<length>": "Length",
    "<page-size>": "PageSize",
    "<percentage [0,∞]>": "PositivePercentage",
    "<string>": "String",
    "<unicode-range-token>#": "UnicodeRangeTokens",
    "<counter-style-system>": "CounterStyleSystem",
    "<counter-style-negative>": "CounterStyleNegative",
    "<symbol>": "Symbol",
    "<symbol>+": "Symbols",
    "<counter-style-range>": "CounterStyleRange",
    "<counter-style-pad>": "CounterStylePad",
    "<counter-style-name>": "CounterStyleName",
    "<counter-style-additive-symbols>": "CounterStyleAdditiveSymbols",
}


def generate_syntax_list(out: TextIO, syntax: list) -> None:
    for entry in syntax:
        if entry.startswith("<'"):
            # Property reference like <'font-feature-settings'>
            property_name = title_casify(entry[2:-2])
            out.write(f"""
            metadata.syntax.empend(PropertyID::{property_name});
""")
        elif entry.startswith("<"):
            # Value type
            # FIXME: Actually parse the grammar, instead of hard-coding the options!
            if entry not in VALUE_TYPE_NAMES:
                print(f"Unrecognized value type: `{entry}`", file=sys.stderr)
                sys.exit(1)
            value_type = VALUE_TYPE_NAMES[entry]
            out.write(f"""
            metadata.syntax.empend(DescriptorMetadata::ValueType::{value_type});
""")
        elif entry == "crop || cross":
            # FIXME: This is extra hacky.
            out.write("""
            metadata.syntax.empend(DescriptorMetadata::ValueType::CropOrCross);
""")
        else:
            # Keyword
            out.write(f"""
            metadata.syntax.empend(Keyword::{title_casify(entry)});
""")


def write_implementation_file(out: TextIO, at_rules_data: dict, all_descriptors: list) -> None:
    at_rule_count = len(at_rules_data)
    descriptor_count = len(all_descriptors)

    out.write("""
#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

FlyString to_string(AtRuleID at_rule_id)
{
    switch (at_rule_id) {
""")

    for at_rule_name in at_rules_data:
        out.write(f"""
    case AtRuleID::{title_casify(at_rule_name)}:
        return "@{at_rule_name}"_fly_string;
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

Optional<DescriptorID> descriptor_id_from_string(AtRuleID at_rule_id, StringView string)
{
    switch (at_rule_id) {
""")

    for at_rule_name, at_rule in at_rules_data.items():
        out.write(f"""
    case AtRuleID::{title_casify(at_rule_name)}:
""")

        if "custom-descriptors" in at_rule:
            out.write("""
        if (is_a_custom_property_name_string(string))
            return DescriptorID::Custom;
""")

        descriptors = at_rule["descriptors"]
        for descriptor_name, descriptor in descriptors.items():
            alias_for = descriptor.get("legacy-alias-for")
            result_name = title_casify(alias_for) if alias_for is not None else title_casify(descriptor_name)
            out.write(f"""
        if (string.equals_ignoring_ascii_case("{descriptor_name}"sv))
            return DescriptorID::{result_name};
""")

        out.write("""
        break;
""")

    out.write("""
    }
    return {};
}

FlyString to_string(DescriptorID descriptor_id)
{
    switch (descriptor_id) {
""")

    for descriptor_name in all_descriptors:
        out.write(f"""
    case DescriptorID::{title_casify(descriptor_name)}:
        return "{descriptor_name}"_fly_string;
""")

    out.write("""
    case DescriptorID::Custom:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

bool at_rule_supports_descriptor(AtRuleID at_rule_id, DescriptorID descriptor_id)
{
    switch (at_rule_id) {
""")

    for at_rule_name, at_rule in at_rules_data.items():
        out.write(f"""
    case AtRuleID::{title_casify(at_rule_name)}:
        switch (descriptor_id) {{
""")

        descriptors = at_rule["descriptors"]
        for descriptor_name, descriptor in descriptors.items():
            if is_legacy_alias(descriptor):
                continue
            out.write(f"        case DescriptorID::{title_casify(descriptor_name)}:\n")

        if "custom-descriptors" in at_rule:
            out.write("""
        case DescriptorID::Custom:
""")

        out.write("""
            return true;
        default:
            return false;
        }
""")

    out.write(f"""
    }}
    VERIFY_NOT_REACHED();
}}


RefPtr<StyleValue const> descriptor_initial_value(AtRuleID at_rule_id, DescriptorID descriptor_id)
{{
    if (!at_rule_supports_descriptor(at_rule_id, descriptor_id))
        return nullptr;

    static Array<Array<RefPtr<StyleValue const>, {descriptor_count}>, {at_rule_count}> initial_values;
    if (auto initial_value = initial_values[to_underlying(at_rule_id)][to_underlying(descriptor_id)])
        return initial_value.release_nonnull();

    // Lazily parse initial values as needed.

    Parser::ParsingParams parsing_params;
    switch (at_rule_id) {{
""")

    for at_rule_name, at_rule in at_rules_data.items():
        at_rule_titlecase = title_casify(at_rule_name)
        out.write(f"""
    case AtRuleID::{at_rule_titlecase}:
        switch (descriptor_id) {{
""")

        descriptors = at_rule["descriptors"]
        for descriptor_name, descriptor in descriptors.items():
            if is_legacy_alias(descriptor):
                continue
            descriptor_titlecase = title_casify(descriptor_name)
            initial_value = descriptor.get("initial")
            if initial_value is not None:
                out.write(f"""
        case DescriptorID::{descriptor_titlecase}: {{
            auto parsed_value = parse_css_descriptor(parsing_params, AtRuleID::{at_rule_titlecase}, DescriptorNameAndID::from_id(DescriptorID::{descriptor_titlecase}), "{initial_value}"sv);
            VERIFY(!parsed_value.is_null());
            auto initial_value = parsed_value.release_nonnull();
            initial_values[to_underlying(at_rule_id)][to_underlying(descriptor_id)] = initial_value;
            return initial_value;
        }}
""")
            else:
                out.write(f"""
        case DescriptorID::{descriptor_titlecase}:
            return nullptr;
""")

        out.write("""
        default:
            VERIFY_NOT_REACHED();
        }
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

DescriptorMetadata get_descriptor_metadata(AtRuleID at_rule_id, DescriptorID descriptor_id)
{
    switch (at_rule_id) {
""")

    for at_rule_name, at_rule in at_rules_data.items():
        out.write(f"""
    case AtRuleID::{title_casify(at_rule_name)}:
        switch (descriptor_id) {{
""")

        descriptors = at_rule["descriptors"]
        for descriptor_name, descriptor in descriptors.items():
            if is_legacy_alias(descriptor):
                continue
            descriptor_titlecase = title_casify(descriptor_name)
            out.write(f"""
        case DescriptorID::{descriptor_titlecase}: {{
            DescriptorMetadata metadata;
""")
            generate_syntax_list(out, descriptor["syntax"])
            allow_arbitrary = "true" if descriptor.get("allow-arbitrary-substitution-functions", False) else "false"
            out.write(f"""
            metadata.allow_arbitrary_substitution_functions = {allow_arbitrary};

            return metadata;
        }}
""")

        if "custom-descriptors" in at_rule:
            custom_descriptors = at_rule["custom-descriptors"]
            out.write("""
        case DescriptorID::Custom: {
            DescriptorMetadata metadata;
""")
            generate_syntax_list(out, custom_descriptors["syntax"])
            allow_arbitrary = (
                "true" if custom_descriptors.get("allow-arbitrary-substitution-functions", False) else "false"
            )
            out.write(f"""
            metadata.allow_arbitrary_substitution_functions = {allow_arbitrary};

            return metadata;
        }}
""")

        out.write("""
        default:
            VERIFY_NOT_REACHED();
        }
""")

    out.write("""
    }
    VERIFY_NOT_REACHED();
}

}
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS DescriptorID", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the DescriptorID header file to generate")
    parser.add_argument(
        "-c",
        "--implementation",
        required=True,
        help="Path to the DescriptorID implementation file to generate",
    )
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        at_rules_data = json.load(input_file)

    all_descriptors = collect_all_descriptors(at_rules_data)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, at_rules_data, all_descriptors)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, at_rules_data, all_descriptors)


if __name__ == "__main__":
    main()
