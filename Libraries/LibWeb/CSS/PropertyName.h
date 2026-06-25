/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

inline bool is_invalid_custom_property_name_string(StringView string)
{
    // https://drafts.csswg.org/css-variables-2/#typedef-custom-property-name
    // The <custom-property-name> production corresponds to this: it’s defined as any <dashed-ident>
    // (a valid identifier that starts with two dashes), except -- itself, which is reserved for future use by CSS.
    return string == "--"sv;
}

// https://drafts.css-houdini.org/css-typed-om-1/#custom-property-name-string
inline bool is_a_custom_property_name_string(StringView string)
{
    // A string is a custom property name string if it starts with two dashes (U+002D HYPHEN-MINUS), like --foo.
    return string.starts_with("--"sv) && !is_invalid_custom_property_name_string(string);
}

// https://drafts.csswg.org/css-variables-2/#custom-property
inline bool is_a_custom_property_name_string(Utf16View string)
{
    // A custom property is any property whose name starts with two dashes (U+002D HYPHEN-MINUS), like --foo.
    // The <custom-property-name> production corresponds to this: it’s defined as any <dashed-ident> (a valid
    // identifier that starts with two dashes), except -- itself, which is reserved for future use by CSS.
    return string.length_in_code_units() > 2
        && string.code_unit_at(0) == '-'
        && string.code_unit_at(1) == '-';
}

// https://drafts.css-houdini.org/css-typed-om-1/#valid-css-property
inline bool is_a_valid_css_property(StringView string)
{
    // A string is a valid CSS property if it is a custom property name string, or is a CSS property name recognized by
    // the user agent.
    return is_a_custom_property_name_string(string) || property_id_from_string(string).has_value();
}

inline bool is_a_valid_css_property(Utf16FlyString const& string)
{
    if (is_a_custom_property_name_string(string))
        return true;
    if (!string.is_ascii())
        return false;
    auto string_data = string.to_utf16_string();
    return property_id_from_string(string_data.ascii_view()).has_value();
}

}
