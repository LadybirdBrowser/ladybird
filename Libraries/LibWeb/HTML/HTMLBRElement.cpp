/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLBRElement);

HTMLBRElement::HTMLBRElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLBRElement::~HTMLBRElement() = default;

CSS::ElementBoxKind HTMLBRElement::box_kind() const
{
    return CSS::ElementBoxKind::Break;
}

bool HTMLBRElement::is_presentational_hint(Utf16FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::clear;
}

void HTMLBRElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);
    for_each_attribute([&](Utf16FlyString const& name, Utf16View value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#phrasing-content-3
        if (name == HTML::AttributeNames::clear) {
            if (value.equals_ignoring_ascii_case(u"left"sv))
                properties.append({ .property_id = CSS::PropertyID::Clear, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Left) });
            else if (value.equals_ignoring_ascii_case(u"right"sv))
                properties.append({ .property_id = CSS::PropertyID::Clear, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Right) });
            else if (value.equals_ignoring_ascii_case(u"all"sv) || value.equals_ignoring_ascii_case(u"both"sv))
                properties.append({ .property_id = CSS::PropertyID::Clear, .value = CSS::KeywordStyleValue::create(CSS::Keyword::Both) });
        }
    });
}

bool HTMLBRElement::represents_empty_line(Layout::BegunRead const& read) const
{
    auto const* layout_node = this->layout_node(read);
    if (!layout_node || !layout_node->containing_block())
        return false;
    return Layout::RustFFI::layout_script_forced_break_ends_empty_line(layout_node->document_host(),
        Layout::Node::slot_id(layout_node->containing_block()), Layout::Node::slot_id(layout_node));
}

}
