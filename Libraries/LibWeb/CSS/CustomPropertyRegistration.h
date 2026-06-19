/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-properties-values-api/#custom-property-registration
// A registered custom property has a custom property registration that contains all the data necessary to treat it
// like a real property. It’s a struct consisting of:
struct CustomPropertyRegistration {
    // - a property name (a custom property name string)
    Utf16FlyString property_name;

    // - a syntax (a syntax string)
    //   NB: Spec actually wants this to be a parsed value, and that's what's most useful to us.
    //       See https://drafts.css-houdini.org/css-properties-values-api/#register-a-custom-property
    NonnullRefPtr<Parser::SyntaxNode> syntax;

    // - an inherit flag (a boolean)
    bool inherit;

    // - optionally, an initial value (a string which successfully parses according to the syntax)
    //   NB: Spec actually wants this to be a parsed value, and that's what's most useful to us.
    //       See https://drafts.css-houdini.org/css-properties-values-api/#register-a-custom-property
    RefPtr<StyleValue const> initial_value;
    RefPtr<StyleValue const> computed_initial_value { nullptr };
};

NonnullRefPtr<StyleValue const> compute_registered_custom_property_value(CustomPropertyRegistration const&, NonnullRefPtr<StyleValue const>, ComputationContext const&);
NonnullRefPtr<StyleValue const> compute_registered_custom_property_initial_value(DOM::Document const&, CustomPropertyRegistration const&);

inline bool operator==(CustomPropertyRegistration const& a, CustomPropertyRegistration const& b)
{
    if (a.property_name != b.property_name)
        return false;
    if (a.syntax.ptr() != b.syntax.ptr() && !a.syntax->equals(*b.syntax))
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
