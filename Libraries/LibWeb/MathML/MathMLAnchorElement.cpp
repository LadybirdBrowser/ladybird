/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MathMLAnchorElement.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/MathML/AttributeNames.h>
#include <LibWeb/MathML/MathMLAnchorElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLAnchorElement);

MathMLAnchorElement::MathMLAnchorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

MathMLAnchorElement::~MathMLAnchorElement() = default;

void MathMLAnchorElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::href && !namespace_.has_value())
        href_content_attribute_changed();
}

bool MathMLAnchorElement::has_activation_behavior() const
{
    return creates_a_hyperlink();
}

void MathMLAnchorElement::activation_behavior(DOM::Event const& event)
{
    if (event.is_trusted() && is_editable_or_editing_host())
        return;

    activate_the_hyperlink(event);
}

bool MathMLAnchorElement::is_focusable() const
{
    return (Base::is_focusable() || has_attribute(AttributeNames::href)) && meets_focusable_area_rendering_requirements();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 MathMLAnchorElement::default_tab_index_value() const
{
    // 3. Return 0 if this is [...] MathML a element [...]; otherwise −1.
    return 0;
}

}
