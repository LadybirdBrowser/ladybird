/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSStyleDeclarationPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleDeclaration);

CSSStyleDeclaration::CSSStyleDeclaration(JS::Realm& realm, Computed computed, Readonly readonly)
    : PlatformObject(realm)
    , m_computed(computed == Computed::Yes)
    , m_readonly(readonly == Readonly::Yes)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
    };
}

void CSSStyleDeclaration::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleDeclaration);
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

    // 4. Set declaration block’s updating flag.
    set_is_updating(true);

    // 5. Set an attribute value for owner node using "style" and the result of serializing declaration block.
    MUST(owner_node()->element().set_attribute(HTML::AttributeNames::style, serialized()));

    // 6. Unset declaration block’s updating flag.
    set_is_updating(false);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
String CSSStyleDeclaration::css_text() const
{
    // 1. If the computed flag is set, then return the empty string.
    if (is_computed())
        return {};

    // 2. Return the result of serializing the declarations.
    return serialized();
}

Optional<JS::Value> CSSStyleDeclaration::item_value(size_t index) const
{
    auto value = item(index);
    if (value.is_empty())
        return {};

    return JS::PrimitiveString::create(vm(), value);
}

void CSSStyleDeclaration::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent_rule);
    if (m_owner_node.has_value())
        m_owner_node->visit(visitor);
}

}
