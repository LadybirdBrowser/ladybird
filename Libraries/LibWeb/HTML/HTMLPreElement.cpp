/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/HTML/HTMLPreElement.h>
#include <LibWeb/HTML/Numbers.h>

namespace Web::HTML {

HTMLPreElement::HTMLPreElement(DOM::VtableProbe probe)
    : HTMLElement(probe)
{
}

bool HTMLPreElement::is_presentational_hint(Utf16FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::wrap;
}

void HTMLPreElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    HTMLElement::apply_presentational_hints(properties);

    for_each_attribute([&](Utf16FlyString const& name, Utf16View) {
        if (name == HTML::AttributeNames::wrap)
            properties.append({ .property_id = CSS::PropertyID::TextWrapMode, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Wrap) });
    });
}

}
