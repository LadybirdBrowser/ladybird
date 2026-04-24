#!/usr/bin/env python3

# Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
# Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import copy
import json
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.utils import camel_casify
from Utils.utils import snake_casify
from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum


def is_legacy_alias(prop: dict) -> bool:
    return "legacy-alias-for" in prop


def verify_alphabetical(data: dict, path: str) -> None:
    most_recent_name = ""
    for name in data:
        if name < most_recent_name:
            print(
                f"`{name}` is in the wrong position in `{path}`. Please keep this list alphabetical!", file=sys.stderr
            )
            sys.exit(1)
        most_recent_name = name


def replace_logical_aliases(properties: dict, logical_property_groups: dict) -> None:
    # Grab the first physical property in each logical group, to use as the template
    first_physical_property_in_logical_group = {}
    for group_name, group in logical_property_groups.items():
        physical = group["physical"]
        if not physical:
            raise ValueError(f"No physical properties in group {group_name}")
        first_physical_property_in_logical_group[group_name] = next(iter(physical.values()))

    logical_aliases = {}
    for name, value in properties.items():
        logical_alias_for = value.get("logical-alias-for")
        if logical_alias_for is not None:
            group_name = logical_alias_for.get("group")
            if group_name is None:
                print(f"Logical alias '{name}' is missing its group", file=sys.stderr)
                sys.exit(1)
            physical_property_name = first_physical_property_in_logical_group.get(group_name)
            if physical_property_name is None:
                print(f"Logical property group '{group_name}' not found! (Property: '{name}')", file=sys.stderr)
                sys.exit(1)
            logical_aliases[name] = physical_property_name

    for name, alias in logical_aliases.items():
        if alias not in properties:
            print(f"No property '{alias}' found for logical alias '{name}'", file=sys.stderr)
            sys.exit(1)
        alias_object = copy.deepcopy(properties[alias])
        # Copy over anything the logical property overrides
        for key, val in properties[name].items():
            alias_object[key] = val
        # Quirks don't carry across to logical aliases
        alias_object.pop("quirks", None)
        properties[name] = alias_object


def populate_all_property_longhands(properties: dict) -> None:
    all_entry = properties.get("all")
    if all_entry is None:
        raise ValueError("Missing 'all' property")

    for name, value in properties.items():
        if "longhands" in value or "legacy-alias-for" in value or name == "direction" or name == "unicode-bidi":
            continue
        all_entry["longhands"].append(name)


def is_animatable_property(properties: dict, property_name: str) -> bool:
    prop = properties[property_name]
    animation_type = prop.get("animation-type")
    if animation_type is not None:
        return animation_type != "none"
    if "longhands" not in prop:
        print(f"Property '{property_name}' must specify either 'animation-type' or 'longhands'", file=sys.stderr)
        sys.exit(1)
    for subproperty_name in prop["longhands"]:
        if is_animatable_property(properties, subproperty_name):
            return True
    return False


def generate_bounds_checking_function(
    out: TextIO, properties: dict, css_type_name: str, type_name: str, value_getter: str = ""
) -> None:
    out.write(f"""
bool property_accepts_{css_type_name}(PropertyID property_id, [[maybe_unused]] {type_name} const& value)
{{
    switch (property_id) {{
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        valid_types = value.get("valid-types")
        if not valid_types:
            continue
        for valid_type in valid_types:
            type_and_range = valid_type.split(" ")
            if type_and_range[0] != css_type_name:
                continue

            out.write(f"""
    case PropertyID::{title_casify(name)}:
        return """)

            if len(type_and_range) > 1:
                range_str = type_and_range[1]
                if not (range_str.startswith("[") and range_str.endswith("]") and "," in range_str):
                    raise ValueError(f"Bad range: {range_str}")
                comma_index = range_str.index(",")
                min_value_string = range_str[1:comma_index]
                max_value_string = range_str[comma_index + 1 : -1]

                if min_value_string == "-∞":
                    min_value_string = ""
                if max_value_string == "∞":
                    max_value_string = ""

                if not min_value_string and not max_value_string:
                    out.write("true;\n")
                    break

                parts = []
                if min_value_string:
                    parts.append(f"{value_getter} >= {min_value_string}")
                if max_value_string:
                    parts.append(f"{value_getter} <= {max_value_string}")
                out.write(" && ".join(parts))
                out.write(";\n")
            else:
                out.write("true;\n")
            break

    out.write("""
    default:
        return false;
    }
}
""")


def format_numeric_range(type_name: str, min_val: str, max_val: str) -> tuple[str, str]:
    if type_name == "integer":
        min_str = "AK::NumericLimits<i32>::min()" if min_val == "-∞" else min_val
        max_str = "AK::NumericLimits<i32>::max()" if max_val == "∞" else max_val
    else:
        min_str = "AK::NumericLimits<float>::lowest()" if min_val == "-∞" else min_val
        max_str = "AK::NumericLimits<float>::max()" if max_val == "∞" else max_val
    return min_str, max_str


def write_header_file(out: TextIO, properties: dict, logical_property_groups: dict) -> None:
    property_id_underlying_type = underlying_type_for_enum(len(properties))
    logical_property_group_underlying_type = underlying_type_for_enum(len(logical_property_groups))

    out.write(f"""
#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/CSS/NumericRange.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {{

enum class PropertyID : {property_id_underlying_type} {{
    Custom,
""")

    shorthand_property_ids = []
    inherited_longhand_property_ids = []
    noninherited_longhand_property_ids = []

    for name, value in properties.items():
        # Legacy aliases don't get a PropertyID
        if is_legacy_alias(value):
            continue
        inherited = value.get("inherited")
        if "longhands" in value:
            if inherited is not None:
                print(f"Property '{name}' with longhands cannot specify 'inherited'", file=sys.stderr)
                sys.exit(1)
            shorthand_property_ids.append(name)
        else:
            if inherited is None:
                print(f"Property '{name}' is missing 'inherited'", file=sys.stderr)
                sys.exit(1)
            if inherited:
                inherited_longhand_property_ids.append(name)
            else:
                noninherited_longhand_property_ids.append(name)

    first_property_id = shorthand_property_ids[0]
    last_property_id = noninherited_longhand_property_ids[-1]
    first_longhand_property_id = inherited_longhand_property_ids[0]
    last_longhand_property_id = noninherited_longhand_property_ids[-1]
    first_inherited_property_id = inherited_longhand_property_ids[0]
    last_inherited_property_id = inherited_longhand_property_ids[-1]

    for ids in (shorthand_property_ids, inherited_longhand_property_ids, noninherited_longhand_property_ids):
        for name in ids:
            out.write(f"""
        {title_casify(name)},
""")

    # FIXME: property_accepts_{number,percentage}() and property_accepted_ranges_by_value_type provide the same data, we should consolidate them.
    out.write(f"""
}};

enum class AnimationType {{
    Discrete,
    ByComputedValue,
    RepeatableList,
    Custom,
    None,
}};
AnimationType animation_type_from_longhand_property(PropertyID);
bool is_animatable_property(PropertyID);

Optional<PropertyID> property_id_from_camel_case_string(StringView);
WEB_API Optional<PropertyID> property_id_from_string(StringView);
[[nodiscard]] WEB_API FlyString const& string_from_property_id(PropertyID);
[[nodiscard]] FlyString const& camel_case_string_from_property_id(PropertyID);
WEB_API bool is_inherited_property(PropertyID);
NonnullRefPtr<StyleValue const> property_initial_value(PropertyID);

enum class PropertyMultiplicity {{
    Single,
    List,
    CoordinatingList,
}};
PropertyMultiplicity property_multiplicity(PropertyID);
bool property_is_single_valued(PropertyID);
bool property_is_list_valued(PropertyID);

bool property_accepts_type(PropertyID, ValueType);
NumericRangesByValueType property_accepted_ranges_by_value_type(PropertyID);
bool property_accepts_keyword(PropertyID, Keyword);
Optional<Keyword> resolve_legacy_value_alias(PropertyID, Keyword);
Optional<ValueType> property_resolves_percentages_relative_to(PropertyID);
Vector<StringView> property_custom_ident_blacklist(PropertyID);

// These perform range-checking, but are also safe to call with properties that don't accept that type. (They'll just return false.)
bool property_accepts_angle(PropertyID, Angle const&);
bool property_accepts_flex(PropertyID, Flex const&);
bool property_accepts_frequency(PropertyID, Frequency const&);
bool property_accepts_integer(PropertyID, i64 const&);
bool property_accepts_length(PropertyID, Length const&);
bool property_accepts_number(PropertyID, double const&);
bool property_accepts_percentage(PropertyID, Percentage const&);
bool property_accepts_resolution(PropertyID, Resolution const&);
bool property_accepts_time(PropertyID, Time const&);

bool property_is_shorthand(PropertyID);
Vector<PropertyID> const& longhands_for_shorthand(PropertyID);
Vector<PropertyID> const& expanded_longhands_for_shorthand(PropertyID);
bool property_maps_to_shorthand(PropertyID);
Vector<PropertyID> const& shorthands_for_longhand(PropertyID);
Vector<PropertyID> const& property_computation_order();
bool property_is_positional_value_list_shorthand(PropertyID);

bool property_requires_computation_with_inherited_value(PropertyID);
bool property_requires_computation_with_initial_value(PropertyID);
bool property_requires_computation_with_cascaded_value(PropertyID);

size_t property_maximum_value_count(PropertyID);

bool property_affects_layout(PropertyID);
bool property_affects_stacking_context(PropertyID);
bool property_needs_layout_for_getcomputedstyle(PropertyID);
bool property_needs_layout_node_for_resolved_value(PropertyID);

constexpr PropertyID first_property_id = PropertyID::{title_casify(first_property_id)};
constexpr PropertyID last_property_id = PropertyID::{title_casify(last_property_id)};
constexpr PropertyID first_inherited_property_id = PropertyID::{title_casify(first_inherited_property_id)};
constexpr PropertyID last_inherited_property_id = PropertyID::{title_casify(last_inherited_property_id)};
constexpr PropertyID first_longhand_property_id = PropertyID::{title_casify(first_longhand_property_id)};
constexpr PropertyID last_longhand_property_id = PropertyID::{title_casify(last_longhand_property_id)};
constexpr size_t number_of_longhand_properties = to_underlying(last_longhand_property_id) - to_underlying(first_longhand_property_id) + 1;

enum class Quirk {{
    // https://quirks.spec.whatwg.org/#the-hashless-hex-color-quirk
    HashlessHexColor,
    // https://quirks.spec.whatwg.org/#the-unitless-length-quirk
    UnitlessLength,
}};
bool property_has_quirk(PropertyID, Quirk);

struct LogicalAliasMappingContext {{
    WritingMode writing_mode;
    Direction direction;
    // TODO: text-orientation
}};
bool property_is_logical_alias(PropertyID);
PropertyID map_logical_alias_to_physical_property(PropertyID logical_property_id, LogicalAliasMappingContext const&);
PropertyID map_physical_property_to_logical_alias(PropertyID physical_property_id, LogicalAliasMappingContext const&);

enum class LogicalPropertyGroup : {logical_property_group_underlying_type} {{
""")

    for name in logical_property_groups:
        out.write(f"""
    {title_casify(name)},
""")

    out.write("""
};

Optional<LogicalPropertyGroup> logical_property_group_for_property(PropertyID);

} // namespace Web::CSS

namespace AK {

template<>
struct Formatter<Web::CSS::PropertyID> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::PropertyID const& property_id)
    {
        return Formatter<StringView>::format(builder, Web::CSS::string_from_property_id(property_id));
    }
};
} // namespace AK
""")


def write_implementation_file(out: TextIO, properties: dict, logical_property_groups: dict, enum_names: list) -> None:
    out.write("""
#include <AK/Assertions.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

static auto generate_camel_case_property_table()
{
    HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> table;
""")

    for name, value in properties.items():
        legacy_alias_for = value.get("legacy-alias-for")
        target = title_casify(legacy_alias_for) if legacy_alias_for is not None else title_casify(name)
        out.write(f"""
    table.set("{camel_casify(name)}"sv, PropertyID::{target});
""")

    out.write("""
    return table;
}

static HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> const camel_case_properties_table = generate_camel_case_property_table();

Optional<PropertyID> property_id_from_camel_case_string(StringView string)
{
    return camel_case_properties_table.get(string);
}

static auto generate_properties_table()
{
    HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> table;
""")

    for name, value in properties.items():
        legacy_alias_for = value.get("legacy-alias-for")
        target = title_casify(legacy_alias_for) if legacy_alias_for is not None else title_casify(name)
        out.write(f"""
    table.set("{name}"sv, PropertyID::{target});
""")

    out.write("""
    return table;
}

static HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> const properties_table = generate_properties_table();

Optional<PropertyID> property_id_from_string(StringView string)
{
    if (is_a_custom_property_name_string(string))
        return PropertyID::Custom;

    return properties_table.get(string);
}

FlyString const& string_from_property_id(PropertyID property_id) {
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        out.write(f"""
    case PropertyID::{title_casify(name)}: {{
        static FlyString name = "{name}"_fly_string;
        return name;
    }}
""")

    out.write("""
    default: {
        static FlyString invalid_property_id_string = "(invalid CSS::PropertyID)"_fly_string;
        return invalid_property_id_string;
    }
    }
}

FlyString const& camel_case_string_from_property_id(PropertyID property_id) {
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        out.write(f"""
    case PropertyID::{title_casify(name)}: {{
        static FlyString name = "{camel_casify(name)}"_fly_string;
        return name;
    }}
""")

    out.write("""
    default: {
        static FlyString invalid_property_id_string = "(invalid CSS::PropertyID)"_fly_string;
        return invalid_property_id_string;
    }
    }
}

AnimationType animation_type_from_longhand_property(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue

        if "longhands" in value:
            if "animation-type" in value:
                print(f"Property '{name}' with longhands cannot specify 'animation-type'", file=sys.stderr)
                sys.exit(1)
            out.write(f"""
    case PropertyID::{title_casify(name)}:
        VERIFY_NOT_REACHED();
""")
            continue

        animation_type = value.get("animation-type")
        if animation_type is None:
            print(f"No animation-type specified for property '{name}'", file=sys.stderr)
            sys.exit(1)
        out.write(f"""
    case PropertyID::{title_casify(name)}:
        return AnimationType::{title_casify(animation_type)};
""")

    out.write("""
    default:
        return AnimationType::None;
    }
}

bool is_animatable_property(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if is_animatable_property(properties, name):
            out.write(f"    case PropertyID::{title_casify(name)}:\n")
    out.write("""
        return true;
    default:
        return false;
    }
}

bool is_inherited_property(PropertyID property_id)
{
    if (property_id >= first_inherited_property_id && property_id <= last_inherited_property_id)
        return true;
    return false;
}

bool property_affects_layout(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        affects_layout = value.get("affects-layout", True)
        if affects_layout:
            out.write(f"    case PropertyID::{title_casify(name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

bool property_affects_stacking_context(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if value.get("affects-stacking-context", False):
            out.write(f"    case PropertyID::{title_casify(name)}:\n")
    out.write("""
        return true;
    default:
        return false;
    }
}

bool property_needs_layout_for_getcomputedstyle(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if value.get("needs-layout-for-getcomputedstyle", False):
            out.write(f"    case PropertyID::{title_casify(name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

bool property_needs_layout_node_for_resolved_value(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if value.get("needs-layout-node-for-resolved-value", False):
            out.write(f"    case PropertyID::{title_casify(name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

NonnullRefPtr<StyleValue const> property_initial_value(PropertyID property_id)
{
    static Array<RefPtr<StyleValue const>, to_underlying(last_property_id) + 1> initial_values;
    if (auto initial_value = initial_values[to_underlying(property_id)])
        return initial_value.release_nonnull();

    // Lazily parse initial values as needed.
    // This ensures the shorthands will always be able to get the initial values of their longhands.
    // This also now allows a longhand have its own longhand (like background-position-x).

    Parser::ParsingParams parsing_params;
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "initial" not in value:
            print(f"No initial value specified for property '{name}'", file=sys.stderr)
            sys.exit(1)
        initial_value_string = value["initial"]
        title = title_casify(name)
        out.write(f"""        case PropertyID::{title}:
        {{
            auto parsed_value = parse_css_value(parsing_params, "{initial_value_string}"sv, PropertyID::{title});
            VERIFY(!parsed_value.is_null());
            auto initial_value = parsed_value.release_nonnull();
            initial_values[to_underlying(PropertyID::{title})] = initial_value;
            return initial_value;
        }}
""")

    out.write("""        default: VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

PropertyMultiplicity property_multiplicity(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        multiplicity = value.get("multiplicity")
        if multiplicity is not None and multiplicity != "single":
            if multiplicity not in ("single", "list", "coordinating-list"):
                print(
                    f"'{multiplicity}' is not a valid value for 'multiplicity'. "
                    "Accepted values are: 'single', 'list', 'coordinating-list'",
                    file=sys.stderr,
                )
                sys.exit(1)
            out.write(f"    case PropertyID::{title_casify(name)}:\n")
            out.write(f"        return PropertyMultiplicity::{title_casify(multiplicity)};\n")

    out.write("""
    default:
        return PropertyMultiplicity::Single;
    }

    VERIFY_NOT_REACHED();
}

bool property_is_single_valued(PropertyID property_id)
{
    return !property_is_list_valued(property_id);
}

bool property_is_list_valued(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        multiplicity = value.get("multiplicity")
        if multiplicity is not None and multiplicity != "single":
            if multiplicity not in ("list", "coordinating-list"):
                print(
                    f"'{multiplicity}' is not a valid value for 'multiplicity'. "
                    "Accepted values are: 'single', 'list', 'coordinating-list'",
                    file=sys.stderr,
                )
                sys.exit(1)
            out.write(f"    case PropertyID::{title_casify(name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

bool property_has_quirk(PropertyID property_id, Quirk quirk)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        quirks = value.get("quirks")
        if quirks:
            out.write(f"""
    case PropertyID::{title_casify(name)}: {{
        switch (quirk) {{
""")
            for quirk in quirks:
                out.write(f"""
        case Quirk::{title_casify(quirk)}:
            return true;
""")
            out.write("""
        default:
            return false;
        }
    }
""")

    out.write("""
    default:
        return false;
    }
}

bool property_accepts_type(PropertyID property_id, ValueType value_type)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        valid_types = value.get("valid-types")
        if not valid_types:
            continue
        out.write(f"""
    case PropertyID::{title_casify(name)}: {{
        switch (value_type) {{
""")
        did_output_accepted_type = False
        for type_entry in valid_types:
            type_name = type_entry.split(" ")[0]
            if type_name in enum_names:
                continue
            out.write(f"        case ValueType::{title_casify(type_name)}:\n")
            did_output_accepted_type = True

        if did_output_accepted_type:
            out.write("            return true;\n")

        out.write("""
        default:
            return false;
        }
    }
""")

    out.write("""
    default:
        return false;
    }
}

NumericRangesByValueType property_accepted_ranges_by_value_type(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        valid_types = value.get("valid-types")
        if not valid_types:
            continue

        ranges_parts = []
        for type_entry in valid_types:
            type_parts = type_entry.split(" ")
            if len(type_parts) < 2:
                continue
            type_name = type_parts[0]
            if type_name == "custom-ident":
                continue
            # Drop brackets: "[-∞,∞]" -> "-∞,∞"
            type_range = type_parts[1][1:-1]
            limits = type_range.split(",")
            if len(limits) != 2:
                raise ValueError(f"Bad range: {type_parts[1]}")
            min_str, max_str = format_numeric_range(type_name, limits[0], limits[1])
            ranges_parts.append(f"{{ ValueType::{title_casify(type_name)}, {{ {min_str}, {max_str} }} }}")

        ranges = ", ".join(ranges_parts)
        out.write(f"""
    case PropertyID::{title_casify(name)}: {{
        return {{ {ranges} }};
    }}""")

    out.write("""
    default: {
        return { };
    }
    }
}

bool property_accepts_keyword(PropertyID property_id, Keyword keyword)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        out.write(f"    case PropertyID::{title_casify(name)}: {{\n")

        valid_identifiers = value.get("valid-identifiers")
        if valid_identifiers:
            out.write("        switch (keyword) {\n")
            for keyword_string in valid_identifiers:
                if ">" in keyword_string:
                    parts = keyword_string.split(">", 1)
                    keyword_titlecase = title_casify(parts[0])
                else:
                    keyword_titlecase = title_casify(keyword_string)
                out.write(f"        case Keyword::{keyword_titlecase}:\n")
            out.write("""
            return true;
        default:
            break;
        }
""")

        valid_types = value.get("valid-types")
        if valid_types:
            for valid_type in valid_types:
                type_name = valid_type.split(" ")[0]
                if type_name not in enum_names:
                    continue
                out.write(f"""
        if (keyword_to_{snake_casify(type_name)}(keyword).has_value())
            return true;
""")

        out.write("""
        return false;
    }
""")

    out.write("""
    default:
        return false;
    }
}

Optional<Keyword> resolve_legacy_value_alias(PropertyID property_id, Keyword keyword)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        valid_identifiers = value.get("valid-identifiers")
        if not valid_identifiers:
            continue
        has_any_legacy_value_aliases = any(">" in k for k in valid_identifiers)
        if not has_any_legacy_value_aliases:
            continue

        out.write(f"""
    case PropertyID::{title_casify(name)}:
        switch (keyword) {{""")
        for keyword_string in valid_identifiers:
            if ">" not in keyword_string:
                continue
            parts = keyword_string.split(">", 1)
            out.write(f"""
        case Keyword::{title_casify(parts[0])}:
            return Keyword::{title_casify(parts[1])};""")
        out.write("""
        default:
            break;
        }
        break;
""")

    out.write("""
    default:
        break;
    }
    return {};
}

Optional<ValueType> property_resolves_percentages_relative_to(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        resolved_type = value.get("percentages-resolve-to")
        if resolved_type is None:
            continue
        out.write(f"""
    case PropertyID::{title_casify(name)}:
        return ValueType::{title_casify(resolved_type)};
""")

    out.write("""
    default:
        return {};
    }
}

Vector<StringView> property_custom_ident_blacklist(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        valid_types = value.get("valid-types")
        if not valid_types:
            continue
        for valid_type in valid_types:
            type_and_parameters = valid_type.split(" ")
            if type_and_parameters[0] != "custom-ident" or len(type_and_parameters) == 1:
                continue
            if len(type_and_parameters) != 2:
                raise ValueError(f"Bad custom-ident parameters: {valid_type}")
            parameters_string = type_and_parameters[1]
            if not (parameters_string.startswith("![") and parameters_string.endswith("]")):
                raise ValueError(f"Bad custom-ident parameters: {parameters_string}")
            blacklisted_keywords = parameters_string[2:-1].split(",")

            out.write(f"""
    case PropertyID::{title_casify(name)}:
        return Vector {{ """)
            for keyword in blacklisted_keywords:
                out.write(f'"{keyword}"sv, ')
            out.write("};\n")

    out.write("""
    default:
        return {};
    }
}

size_t property_maximum_value_count(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "max-values" in value:
            max_values = value["max-values"]
            out.write(f"""
    case PropertyID::{title_casify(name)}:
        return {max_values};
""")

    out.write("""
    default:
        return 1;
    }
}""")

    generate_bounds_checking_function(out, properties, "angle", "Angle", "value.raw_value()")
    generate_bounds_checking_function(out, properties, "flex", "Flex", "value.raw_value()")
    generate_bounds_checking_function(out, properties, "frequency", "Frequency", "value.raw_value()")
    generate_bounds_checking_function(out, properties, "integer", "i64", "value")
    generate_bounds_checking_function(out, properties, "length", "Length", "value.raw_value()")
    generate_bounds_checking_function(out, properties, "number", "double", "value")
    generate_bounds_checking_function(out, properties, "percentage", "Percentage", "value.value()")
    generate_bounds_checking_function(out, properties, "resolution", "Resolution", "value.raw_value()")
    generate_bounds_checking_function(out, properties, "time", "Time", "value.raw_value()")

    out.write("""
bool property_is_shorthand(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "longhands" in value:
            out.write(f"        case PropertyID::{title_casify(name)}:\n")

    out.write("""
            return true;
        default:
            return false;
        }
}
""")

    def get_longhands(property_id: str) -> list:
        return list(properties[property_id]["longhands"])

    out.write("""
Vector<PropertyID> const& longhands_for_shorthand(PropertyID property_id)
{
    switch (property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "longhands" in value:
            longhands = ", ".join(f"PropertyID::{title_casify(lh)}" for lh in get_longhands(name))
            out.write(f"""
        case PropertyID::{title_casify(name)}: {{
            static Vector<PropertyID> longhands = {{ {longhands} }};
            return longhands;
        }}""")

    out.write("""
        default:
            static Vector<PropertyID> empty_longhands;
            return empty_longhands;
        }
}

Vector<PropertyID> const& expanded_longhands_for_shorthand(PropertyID property_id)
{
    switch (property_id) {
""")

    def get_expanded_longhands(property_id: str) -> list:
        expanded = []
        for longhand_id in get_longhands(property_id):
            prop = properties[longhand_id]
            if "longhands" in prop:
                expanded.extend(get_expanded_longhands(longhand_id))
            else:
                expanded.append(longhand_id)
        return expanded

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "longhands" in value:
            longhands = ", ".join(f"PropertyID::{title_casify(lh)}" for lh in get_expanded_longhands(name))
            out.write(f"""
    case PropertyID::{title_casify(name)}: {{
        static Vector<PropertyID> longhands = {{ {longhands} }};
        return longhands;
    }}""")

    out.write("""
    default: {
        static Vector<PropertyID> empty_longhands;
        return empty_longhands;
    }
    }
}
""")

    # shorthands_for_longhand_map: for each longhand, which shorthand(s) contain it.
    shorthands_for_longhand_map = {}
    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "longhands" in value:
            for longhand_name in value["longhands"]:
                shorthands_for_longhand_map.setdefault(longhand_name, []).append(name)

    out.write("""
bool property_maps_to_shorthand(PropertyID property_id)
{
    switch (property_id) {
""")

    for longhand in shorthands_for_longhand_map.keys():
        out.write(f"        case PropertyID::{title_casify(longhand)}:\n")

    out.write("""
            return true;
        default:
            return false;
        }
}

Vector<PropertyID> const& shorthands_for_longhand(PropertyID property_id)
{
    switch (property_id) {
""")

    def get_shorthands_for_longhand(longhand: str) -> list:
        shorthands = []
        for immediate in shorthands_for_longhand_map.get(longhand, []):
            shorthands.append(immediate)
            if immediate in shorthands_for_longhand_map:
                shorthands.extend(get_shorthands_for_longhand(immediate))

        # https://www.w3.org/TR/cssom/#concept-shorthands-preferred-order
        # Combined sort key:
        # 4. Number of longhand properties (greatest first)
        # 2. Items starting with "-" last
        # 3. Items starting with "-webkit-" before other "-" items
        # 1. Lexicographic
        def sort_key(shorthand):
            num_longhands = len(get_expanded_longhands(shorthand))
            starts_with_dash = shorthand.startswith("-")
            starts_with_webkit = shorthand.startswith("-webkit-")
            # We want descending on num_longhands, so negate.
            # For dash ordering: starts_with_dash True should sort LATER (tuple sort is ascending).
            # Among dashed: -webkit- should be first (True < False? no, False < True).
            # Actually, if both start with dash, the one starting with -webkit- should come first.
            # So (starts_with_dash=True, starts_with_webkit=True) < (starts_with_dash=True, starts_with_webkit=False)
            # means starts_with_webkit=True must be smaller: use NOT starts_with_webkit.
            return (
                -num_longhands,
                starts_with_dash,
                not starts_with_webkit,
                shorthand,
            )

        shorthands.sort(key=sort_key)
        return shorthands

    for longhand in shorthands_for_longhand_map.keys():
        shorthands = ", ".join(f"PropertyID::{title_casify(s)}" for s in get_shorthands_for_longhand(longhand))
        out.write(f"""
    case PropertyID::{title_casify(longhand)}: {{
        static Vector<PropertyID> shorthands = {{ {shorthands} }};
        return shorthands;
    }}""")

    out.write("""
    default: {
        static Vector<PropertyID> empty_shorthands;
        return empty_shorthands;
    }
    }
}
""")

    manually_specified_computation_order = [
        # math-depth is required to compute font-size
        "MathDepth",
        # Font properties are required to absolutize font-relative units used in other properties, including line-height.
        "FontFamily",
        "FontFeatureSettings",
        "FontKerning",
        "FontOpticalSizing",
        "FontSize",
        "FontStyle",
        "FontVariantAlternates",
        "FontVariantCaps",
        "FontVariantEastAsian",
        "FontVariantEmoji",
        "FontVariantLigatures",
        "FontVariantNumeric",
        "FontVariantPosition",
        "FontVariationSettings",
        "FontWeight",
        "FontWidth",
        "TextRendering",
        # line-height is required to absolutize `lh` units used in other properties.
        "LineHeight",
        # color-scheme is included in the generic computation context in order to compute light-dark() color functions
        "ColorScheme",
        # background-image is required to compute the other background-* properties
        "BackgroundImage",
        # text direction and writing mode properties are required to map logical properties to their physical counterparts
        "Direction",
        "WritingMode",
    ]

    out.write("""
Vector<PropertyID> const& property_computation_order() {
    static Vector<PropertyID> order = {
""")
    for property_name in manually_specified_computation_order:
        out.write(f"        PropertyID::{property_name},\n")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "longhands" in value:
            continue
        if title_casify(name) in manually_specified_computation_order:
            continue
        out.write(f"        PropertyID::{title_casify(name)},\n")

    out.write("""
    };

    return order;
}

bool property_is_positional_value_list_shorthand(PropertyID property_id)
{
    switch (property_id)
    {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "positional-value-list-shorthand" in value:
            out.write(f"    case PropertyID::{title_casify(name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}
""")

    properties_requiring_inherited = []
    properties_requiring_initial = []
    properties_requiring_cascaded = []

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        requires_computation = value.get("requires-computation")
        if requires_computation is not None and "longhands" in value:
            print(f"Property '{name}' is a shorthand and cannot have 'requires-computation' set.", file=sys.stderr)
            sys.exit(1)
        if "longhands" in value:
            continue
        if requires_computation is None:
            print(f"Property '{name}' is missing 'requires-computation' field.", file=sys.stderr)
            sys.exit(1)
        if requires_computation == "always":
            properties_requiring_inherited.append(name)
            properties_requiring_initial.append(name)
            properties_requiring_cascaded.append(name)
        elif requires_computation == "non-inherited-value":
            properties_requiring_initial.append(name)
            properties_requiring_cascaded.append(name)
        elif requires_computation == "cascaded-value":
            properties_requiring_cascaded.append(name)
        elif requires_computation != "never":
            print(
                f"Property '{name}' has unrecognized 'requires-computation' value '{requires_computation}'",
                file=sys.stderr,
            )
            sys.exit(1)

    out.write("""
bool property_requires_computation_with_inherited_value(PropertyID property_id)
{
    switch(property_id) {
    """)

    for property_name in properties_requiring_inherited:
        out.write(f"    case PropertyID::{title_casify(property_name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

bool property_requires_computation_with_initial_value(PropertyID property_id)
{
    switch(property_id) {
    """)

    for property_name in properties_requiring_initial:
        out.write(f"    case PropertyID::{title_casify(property_name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

bool property_requires_computation_with_cascaded_value(PropertyID property_id)
{
    switch(property_id) {
    """)

    for property_name in properties_requiring_cascaded:
        out.write(f"    case PropertyID::{title_casify(property_name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

bool property_is_logical_alias(PropertyID property_id)
{
    switch(property_id) {
""")

    for name, value in properties.items():
        if is_legacy_alias(value):
            continue
        if "logical-alias-for" in value:
            out.write(f"    case PropertyID::{title_casify(name)}:\n")

    out.write("""
        return true;
    default:
        return false;
    }
}

PropertyID map_logical_alias_to_physical_property(PropertyID property_id, LogicalAliasMappingContext const& mapping_context)
{
    // https://drafts.csswg.org/css-writing-modes-4/#logical-to-physical
    // FIXME: Note: The used direction depends on the computed writing-mode and text-orientation: in vertical writing
    //              modes, a text-orientation value of upright forces the used direction to ltr.
    auto used_direction = mapping_context.direction;
    switch(property_id) {
""")

    for property_name, value in properties.items():
        if is_legacy_alias(value):
            continue
        logical_alias_for = value.get("logical-alias-for")
        if logical_alias_for is None:
            continue
        group_name = logical_alias_for.get("group")
        mapping = logical_alias_for.get("mapping")
        if group_name is None or mapping is None:
            print(f"Logical alias '{property_name}' is missing either its group or its mapping!", file=sys.stderr)
            sys.exit(1)
        group = logical_property_groups.get(group_name)
        if group is None:
            print(f"Logical alias '{property_name}' has unrecognized group '{group_name}'", file=sys.stderr)
            sys.exit(1)

        def mapped_property(entry_name: str, group=group, group_name=group_name, property_name=property_name) -> str:
            physical = group["physical"]
            if entry_name not in physical:
                print(
                    f"Logical property group '{group_name}' is missing entry for '{entry_name}', "
                    f"requested by property '{property_name}'.",
                    file=sys.stderr,
                )
                sys.exit(1)
            return title_casify(physical[entry_name])

        name_titlecase = title_casify(property_name)
        out.write(f"    case PropertyID::{name_titlecase}:\n")

        if mapping == "block-end":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::{mapped_property("bottom")};
        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl))
            return PropertyID::{mapped_property("left")};
        return PropertyID::{mapped_property("right")};
""")
        elif mapping == "block-size":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::{mapped_property("height")};
        return PropertyID::{mapped_property("width")};
""")
        elif mapping == "block-start":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::{mapped_property("top")};
        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl))
            return PropertyID::{mapped_property("right")};
        return PropertyID::{mapped_property("left")};
""")
        elif mapping == "end-end":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("bottom-right")};
            return PropertyID::{mapped_property("bottom-left")};
        }}

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("bottom-left")};
            return PropertyID::{mapped_property("top-left")};
        }}

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("bottom-right")};
            return PropertyID::{mapped_property("top-right")};
        }}

        if (used_direction == Direction::Ltr)
            return PropertyID::{mapped_property("top-right")};
        return PropertyID::{mapped_property("bottom-right")};
""")
        elif mapping == "end-start":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("bottom-left")};
            return PropertyID::{mapped_property("bottom-right")};
        }}

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("top-left")};
            return PropertyID::{mapped_property("bottom-left")};
        }}

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("top-right")};
            return PropertyID::{mapped_property("bottom-right")};
        }}

        if (used_direction == Direction::Ltr)
            return PropertyID::{mapped_property("bottom-right")};
        return PropertyID::{mapped_property("top-right")};
""")
        elif mapping == "inline-end":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("right")};
            return PropertyID::{mapped_property("left")};
        }}

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl, WritingMode::VerticalLr)) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("bottom")};
            return PropertyID::{mapped_property("top")};
        }}

        if (used_direction == Direction::Ltr)
            return PropertyID::{mapped_property("top")};
        return PropertyID::{mapped_property("bottom")};
""")
        elif mapping == "inline-size":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::{mapped_property("width")};
        return PropertyID::{mapped_property("height")};
""")
        elif mapping == "inline-start":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("left")};
            return PropertyID::{mapped_property("right")};
        }}

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl, WritingMode::VerticalLr)) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("top")};
            return PropertyID::{mapped_property("bottom")};
        }}

        if (used_direction == Direction::Ltr)
            return PropertyID::{mapped_property("bottom")};
        return PropertyID::{mapped_property("top")};
""")
        elif mapping == "start-end":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("top-right")};
            return PropertyID::{mapped_property("top-left")};
        }}

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("bottom-right")};
            return PropertyID::{mapped_property("top-right")};
        }}

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("bottom-left")};
            return PropertyID::{mapped_property("top-left")};
        }}

        if (used_direction == Direction::Ltr)
            return PropertyID::{mapped_property("top-left")};
        return PropertyID::{mapped_property("bottom-left")};
""")
        elif mapping == "start-start":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("top-left")};
            return PropertyID::{mapped_property("top-right")};
        }}

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("top-right")};
            return PropertyID::{mapped_property("bottom-right")};
        }}

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {{
            if (used_direction == Direction::Ltr)
                return PropertyID::{mapped_property("top-left")};
            return PropertyID::{mapped_property("bottom-left")};
        }}
        if (used_direction == Direction::Ltr)
            return PropertyID::{mapped_property("bottom-left")};
        return PropertyID::{mapped_property("top-left")};
""")
        elif mapping == "block-xy":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::{mapped_property("y")};
        return PropertyID::{mapped_property("x")};
""")
        elif mapping == "inline-xy":
            out.write(f"""
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::{mapped_property("x")};
        return PropertyID::{mapped_property("y")};
""")
        else:
            print(f"Logical alias '{property_name}' has unrecognized mapping '{mapping}'", file=sys.stderr)
            sys.exit(1)

    out.write("""
    default:
        VERIFY(!property_is_logical_alias(property_id));
        return property_id;
    }
}

PropertyID map_physical_property_to_logical_alias(PropertyID property_id, LogicalAliasMappingContext const& mapping_context)
{
    switch (property_id) {
""")

    for group in logical_property_groups.values():
        physical_properties = group["physical"]
        logical_properties = group["logical"]

        for physical_property_name in physical_properties.values():
            out.write(f"        case PropertyID::{title_casify(physical_property_name)}:\n")
            for logical_property_name in logical_properties.values():
                out.write(f"""
            if (map_logical_alias_to_physical_property(PropertyID::{title_casify(logical_property_name)}, mapping_context) == property_id)
                return PropertyID::{title_casify(logical_property_name)};
""")
            out.write("            VERIFY_NOT_REACHED();\n")

    out.write("""
        default:
            VERIFY(!logical_property_group_for_property(property_id).has_value() || property_is_logical_alias(property_id));
            return property_id;
    }
}

Optional<LogicalPropertyGroup> logical_property_group_for_property(PropertyID property_id)
{
    switch(property_id) {
""")

    # Build HashMap<String, Vector<String>> of all properties (physical + logical) per group
    logical_property_group_members = {}
    for group_name, mapping in logical_property_groups.items():
        members = logical_property_group_members.setdefault(group_name, [])
        for physical_property in mapping["physical"].values():
            members.append(physical_property)
        for logical_property in mapping["logical"].values():
            members.append(logical_property)

    for group_name, group_properties in logical_property_group_members.items():
        for prop_name in group_properties:
            out.write(f"    case PropertyID::{title_casify(prop_name)}:\n")
        out.write(f"        return LogicalPropertyGroup::{title_casify(group_name)};\n")

    out.write("""
    default:
        return {};
    }
}

} // namespace Web::CSS
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS PropertyID", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the PropertyID header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the PropertyID implementation file to generate"
    )
    parser.add_argument("-j", "--properties-json", required=True, help="Path to the properties JSON file to read from")
    parser.add_argument("-e", "--enums-json", required=True, help="Path to the enums JSON file to read from")
    parser.add_argument(
        "-g", "--groups-json", required=True, help="Path to the logical property groups JSON file to read from"
    )
    args = parser.parse_args()

    with open(args.properties_json, "r", encoding="utf-8") as f:
        properties = json.load(f)
    with open(args.enums_json, "r", encoding="utf-8") as f:
        enums = json.load(f)
    with open(args.groups_json, "r", encoding="utf-8") as f:
        logical_property_groups = json.load(f)

    verify_alphabetical(properties, args.properties_json)
    verify_alphabetical(logical_property_groups, args.groups_json)
    verify_alphabetical(enums, args.enums_json)

    enum_names = list(enums.keys())

    replace_logical_aliases(properties, logical_property_groups)
    populate_all_property_longhands(properties)

    with open(args.header, "w", encoding="utf-8") as f:
        write_header_file(f, properties, logical_property_groups)

    with open(args.implementation, "w", encoding="utf-8") as f:
        write_implementation_file(f, properties, logical_property_groups, enum_names)


if __name__ == "__main__":
    main()
