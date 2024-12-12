/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/PopoverInvokerElement.h>

namespace Web::HTML {

void PopoverInvokerElement::associated_attribute_changed(FlyString const& name, Optional<String> const&, Optional<FlyString> const& namespace_)
{
    // From: https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributess
    // For element reflected targets only: the following attribute change steps, given element, localName, oldValue, value, and namespace,
    // are used to synchronize between the content attribute and the IDL attribute:

    // 1. If localName is not attr or namespace is not null, then return.
    if (name != HTML::AttributeNames::popovertarget || namespace_.has_value())
        return;

    // 2. Set element's explicitly set attr-elements to null.
    m_popover_target_element = nullptr;
}

void PopoverInvokerElement::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_popover_target_element);
}

}
