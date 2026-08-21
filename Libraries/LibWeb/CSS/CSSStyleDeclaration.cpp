/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleDeclaration);

CSSStyleDeclaration::CSSStyleDeclaration(Computed computed, Readonly readonly)
    : m_computed(computed == Computed::Yes)
    , m_readonly(readonly == Readonly::Yes)
{
}

void CSSStyleDeclaration::prepare_to_update_style_attribute()
{
    VERIFY(!is_computed());
    if (!owner_node().has_value())
        return;

    owner_node()->element().prepare_for_inline_style_change();
}

// https://drafts.csswg.org/cssom/#update-style-attribute-for
void CSSStyleDeclaration::update_style_attribute()
{
    // 1. Assert: declaration block’s computed flag is unset.
    VERIFY(!is_computed());

    // 2. Let owner node be declaration block’s owner node.
    // 3. If owner node is null, then return.
    if (!owner_node().has_value())
        return;

    auto& element = owner_node()->element();
    // OPTIMIZATION: Keep the parsed declaration block authoritative and serialize it only when
    //               something observes the textual attribute value.
    if (element.can_defer_inline_style_attribute_update()) {
        element.did_update_inline_style();
        return;
    }

    // 4. Set declaration block’s updating flag.
    set_is_updating(true);

    // 5. Set an attribute value for owner node using "style" and the result of serializing declaration block.
    element.set_attribute_value(HTML::AttributeNames::style, serialized());

    // 6. Unset declaration block’s updating flag.
    set_is_updating(false);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
Utf16String CSSStyleDeclaration::css_text() const
{
    // 1. If the computed flag is set, then return the empty string.
    if (is_computed())
        return {};

    // 2. Return the result of serializing the declarations.
    return serialized();
}

void CSSStyleDeclaration::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_rule);
    if (m_owner_node.has_value())
        m_owner_node->visit(visitor);
}

}
