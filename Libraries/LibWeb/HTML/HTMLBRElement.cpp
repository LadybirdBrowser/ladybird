/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLBRElement.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/VisualLines.h>

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

RefPtr<Layout::Node> HTMLBRElement::create_layout_node(NonnullRefPtr<CSS::ComputedValues const> style)
{
    return make_ref_counted<Layout::BreakNode>(document(), *this, style);
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

// Rendered inline content that would appear before a <br> on its line.
static bool is_rendered_inline_content(DOM::Node const& node)
{
    if (auto const* text = as_if<DOM::Text>(node)) {
        for (auto const& line : collect_visual_lines(*text)) {
            if (!line.fragments.is_empty())
                return true;
        }
        return false;
    }
    if (auto const* element = as_if<DOM::Element>(node)) {
        auto const* layout_node = element->layout_node();
        if (!layout_node)
            return false;
        if (is<Layout::ReplacedBox>(*layout_node))
            return true;
        if (layout_node->display().is_inline_outside() && !layout_node->display().is_flow_inside())
            return true;
    }
    return false;
}

// NB: Layout produces no fragments for <br>, so this walks the DOM back to the start of the containing block or a
//     previous <br>, whichever comes first.
bool HTMLBRElement::represents_empty_line() const
{
    if (!layout_node())
        return false;

    auto const* containing_block = layout_node()->containing_block();
    if (!containing_block)
        return false;
    auto const* containing_block_dom_node = containing_block->dom_node();
    if (!containing_block_dom_node)
        return false;

    for (auto const* previous = previous_in_pre_order(); previous && previous != containing_block_dom_node; previous = previous->previous_in_pre_order()) {
        if (!containing_block_dom_node->is_inclusive_ancestor_of(*previous))
            break;
        if (is<HTMLBRElement>(*previous))
            return true;
        if (is_rendered_inline_content(*previous))
            return false;
    }
    return true;
}

void HTMLBRElement::adjust_computed_style(CSS::ComputedProperties::Builder& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
    else if (!style.display().is_none())
        // AD-HOC: Prevent other display values from applying, so that we always create a BreakNode
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Inline)));
}

}
