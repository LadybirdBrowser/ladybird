/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTableCaptionElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/HTML/HTMLTableCaptionElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLTableCaptionElement);

HTMLTableCaptionElement::HTMLTableCaptionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTableCaptionElement::~HTMLTableCaptionElement() = default;

void HTMLTableCaptionElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTableCaptionElement);
}

// https://html.spec.whatwg.org/multipage/rendering.html#tables-2
void HTMLTableCaptionElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    HTMLElement::apply_presentational_hints(style);
    for_each_attribute([&](auto& name, auto& value) {
        if (name.equals_ignoring_ascii_case("align"sv)) {
            if (value == "bottom"sv)
                style.set_property(CSS::PropertyID::CaptionSide, CSS::CSSKeywordValue::create(CSS::Keyword::Bottom));
        }
    });
}

}
