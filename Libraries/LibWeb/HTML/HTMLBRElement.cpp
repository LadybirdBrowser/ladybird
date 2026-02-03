/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLBRElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Layout/BreakNode.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLBRElement);

HTMLBRElement::HTMLBRElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLBRElement::~HTMLBRElement() = default;

void HTMLBRElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLBRElement);
    Base::initialize(realm);
}

GC::Ptr<Layout::Node> HTMLBRElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::BreakNode>(document(), *this, move(style));
}

bool HTMLBRElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::clear;
}

void HTMLBRElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);
    for_each_attribute([&](auto& name, auto& value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#phrasing-content-3
        if (name == HTML::AttributeNames::clear) {
            if (value.equals_ignoring_ascii_case("left"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Clear, CSS::KeywordStyleValue::create(CSS::Keyword::Left));
            else if (value.equals_ignoring_ascii_case("right"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Clear, CSS::KeywordStyleValue::create(CSS::Keyword::Right));
            else if (value.equals_ignoring_ascii_case("all"sv) || value.equals_ignoring_ascii_case("both"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Clear, CSS::KeywordStyleValue::create(CSS::Keyword::Both));
        }
    });
}

void HTMLBRElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

}
