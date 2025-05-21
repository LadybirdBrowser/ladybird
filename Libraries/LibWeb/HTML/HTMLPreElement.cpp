/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLPreElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/HTML/HTMLPreElement.h>
#include <LibWeb/HTML/Numbers.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLPreElement);

HTMLPreElement::HTMLPreElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLPreElement::~HTMLPreElement() = default;

void HTMLPreElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLPreElement);
    Base::initialize(realm);
}

bool HTMLPreElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::wrap;
}

void HTMLPreElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    HTMLElement::apply_presentational_hints(cascaded_properties);

    for_each_attribute([&](auto const& name, auto const&) {
        if (name.equals_ignoring_ascii_case(HTML::AttributeNames::wrap)) {
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextWrapMode, CSS::CSSKeywordValue::create(CSS::Keyword::Wrap));
        }
    });
}

}
