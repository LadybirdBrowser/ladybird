/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HypotheticalElement.h"
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

DOM::AbstractElement& AbstractOrHypotheticalElement::abstract_element()
{
    return visit(
        [](DOM::AbstractElement& abstract_element) -> DOM::AbstractElement& { return abstract_element; },
        [](HypotheticalElement* hypothetical_element) -> DOM::AbstractElement& { return hypothetical_element->root_element; });
}

DOM::AbstractElement const& AbstractOrHypotheticalElement::abstract_element() const
{
    return const_cast<AbstractOrHypotheticalElement*>(this)->abstract_element();
}

DOM::Document const& AbstractOrHypotheticalElement::document() const
{
    return abstract_element().document();
}

Optional<AbstractOrHypotheticalElement> AbstractOrHypotheticalElement::element_to_inherit_style_from() const
{
    return visit(
        [](DOM::AbstractElement const& abstract_element) -> Optional<AbstractOrHypotheticalElement> {
            auto element_to_inherit_style_from = abstract_element.element_to_inherit_style_from();

            if (!element_to_inherit_style_from.has_value())
                return OptionalNone {};

            return element_to_inherit_style_from.value();
        },
        [](HypotheticalElement* hypothetical_element) -> Optional<AbstractOrHypotheticalElement> {
            return hypothetical_element->parent;
        });
}

Optional<CustomPropertyRegistration const&> AbstractOrHypotheticalElement::get_registered_custom_property(Utf16FlyString const& name) const
{
    return visit(
        [&](DOM::AbstractElement const& abstract_element) {
            return abstract_element.document().get_registered_custom_property(name);
        },
        [&](HypotheticalElement* hypothetical_element) -> Optional<CustomPropertyRegistration const&> {
            return hypothetical_element->custom_property_registry.get(name);
        });
}

RefPtr<StyleValue const> AbstractOrHypotheticalElement::get_custom_property(Utf16FlyString const& name) const
{
    return visit(
        [&](DOM::AbstractElement const& abstract_element) {
            return abstract_element.get_custom_property(name);
        },
        [&](HypotheticalElement* hypothetical_element) -> RefPtr<StyleValue const> {
            if (auto const* property = hypothetical_element->custom_property_data->get(name))
                return property->value;
            return nullptr;
        });
}

}
