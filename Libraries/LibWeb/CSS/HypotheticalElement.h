/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

struct HypotheticalElement;

class AbstractOrHypotheticalElement : public Variant<DOM::AbstractElement, HypotheticalElement*> {
public:
    AbstractOrHypotheticalElement(DOM::AbstractElement abstract_element)
        : Variant<DOM::AbstractElement, HypotheticalElement*>(abstract_element)
    {
    }

    explicit AbstractOrHypotheticalElement(HypotheticalElement& hypothetical_element)
        : Variant<DOM::AbstractElement, HypotheticalElement*>(&hypothetical_element)
    {
    }

    DOM::AbstractElement& abstract_element();
    DOM::AbstractElement const& abstract_element() const;

    DOM::Document const& document() const;

    Optional<AbstractOrHypotheticalElement> element_to_inherit_style_from() const;

    Optional<CustomPropertyRegistration const&> get_registered_custom_property(Utf16FlyString const& name) const;
    RefPtr<StyleValue const> get_custom_property(Utf16FlyString const& name) const;
    RefPtr<CustomPropertyData const> inheritable_custom_property_data() const;
};

// https://drafts.csswg.org/css-mixins/#using-custom-functions
struct HypotheticalElement {
    // NB: While this isn't technically part of the hypothetical element it shares the same scope and is needed to
    //     compute custom properties, so we store it here for convenience.
    HashMap<Utf16FlyString, CustomPropertyRegistration> custom_property_registry;

    DOM::AbstractElement root_element;
    AbstractOrHypotheticalElement parent;
    NonnullRefPtr<CustomPropertyData> custom_property_data;
};

}
