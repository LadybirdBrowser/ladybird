/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

static bool is_invalid_custom_property_name_string(StringView string)
{
    // https://drafts.csswg.org/css-variables-2/#typedef-custom-property-name
    // The <custom-property-name> production corresponds to this: it’s defined as any <dashed-ident>
    // (a valid identifier that starts with two dashes), except -- itself, which is reserved for future use by CSS.
    return string == "--"sv;
}

// https://drafts.css-houdini.org/css-typed-om-1/#custom-property-name-string
static bool is_a_custom_property_name_string(StringView string)
{
    // A string is a custom property name string if it starts with two dashes (U+002D HYPHEN-MINUS), like --foo.
    return string.starts_with("--"sv) && !is_invalid_custom_property_name_string(string);
}

}
