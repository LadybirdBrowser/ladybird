/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNestedDeclarations.h"
#include <LibWeb/Bindings/CSSNestedDeclarationsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleRule.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNestedDeclarations);

GC::Ref<CSSNestedDeclarations> CSSNestedDeclarations::create(JS::Realm& realm, CSSStyleProperties& declaration)
{
    return realm.create<CSSNestedDeclarations>(realm, declaration);
}

CSSNestedDeclarations::CSSNestedDeclarations(JS::Realm& realm, CSSStyleProperties& declaration)
    : CSSRule(realm, Type::NestedDeclarations)
    , m_declaration(declaration)
{
    m_declaration->set_parent_rule(*this);
}

void CSSNestedDeclarations::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSNestedDeclarations);
}

void CSSNestedDeclarations::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_declaration);
    visitor.visit(m_parent_style_rule);
}

CSSStyleDeclaration* CSSNestedDeclarations::style()
{
    return m_declaration;
}

CSSStyleRule const& CSSNestedDeclarations::parent_style_rule() const
{
    if (m_parent_style_rule)
        return *m_parent_style_rule;

    for (auto* parent = parent_rule(); parent; parent = parent->parent_rule()) {
        if (is<CSSStyleRule>(parent)) {
            m_parent_style_rule = static_cast<CSSStyleRule const*>(parent);
            return *m_parent_style_rule;
        }
    }

    dbgln("CSSNestedDeclarations has no parent style rule!");
    VERIFY_NOT_REACHED();
}

String CSSNestedDeclarations::serialized() const
{
    // NOTE: There's no proper spec for this yet, only this note:
    // "The CSSNestedDeclarations rule serializes as if its declaration block had been serialized directly."
    // - https://drafts.csswg.org/css-nesting-1/#ref-for-cssnesteddeclarations%E2%91%A1
    // So, we'll do the simple thing and hope it's good.
    return m_declaration->serialized();
}

void CSSNestedDeclarations::clear_caches()
{
    Base::clear_caches();
    m_parent_style_rule = nullptr;
}

}
