/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/RefPtr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-properties-values-api/#custom-property-registration
// A registered custom property has a custom property registration that contains all the data necessary to treat it
// like a real property. It’s a struct consisting of:
struct CustomPropertyRegistration {
    // - a property name (a custom property name string)
    FlyString property_name;

    // - a syntax (a syntax string)
    String syntax;

    // - an inherit flag (a boolean)
    bool inherit;

    // - optionally, an initial value (a string which successfully parses according to the syntax)
    //   NB: Spec actually wants this to be a parsed value, and that's what's most useful to us.
    //       See https://drafts.css-houdini.org/css-properties-values-api/#register-a-custom-property
    RefPtr<StyleValue const> initial_value;
};

inline bool operator==(CustomPropertyRegistration const& a, CustomPropertyRegistration const& b)
{
    if (a.property_name != b.property_name)
        return false;
    if (a.syntax != b.syntax)
        return false;
    if (a.inherit != b.inherit)
        return false;
    if (a.initial_value.ptr() == b.initial_value.ptr())
        return true;
    if (!a.initial_value || !b.initial_value)
        return false;
    return a.initial_value->equals(*b.initial_value);
}

}
