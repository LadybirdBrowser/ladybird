/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableColElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/HTML/Numbers.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTableColElement);

HTMLTableColElement::HTMLTableColElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableColElement::~HTMLTableColElement() = default;

void HTMLTableColElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableColElement);
}

// https://html.spec.whatwg.org/multipage/tables.html#dom-colgroup-span
unsigned int HTMLTableColElement::span() const
{
    // The span IDL attribute must reflect the content attribute of the same name. It is clamped to the range [1, 1000], and its default value is 1.
    if (auto span_string = get_attribute(HTML::AttributeNames::span); span_string.has_value()) {
        if (auto span = parse_non_negative_integer(*span_string); span.has_value())
            return clamp(*span, 1, 1000);
    }
    return 1;
}

WebIDL::ExceptionOr<void> HTMLTableColElement::set_span(unsigned int value)
{
    return set_attribute(HTML::AttributeNames::span, MUST(String::number(value)));
}

}
