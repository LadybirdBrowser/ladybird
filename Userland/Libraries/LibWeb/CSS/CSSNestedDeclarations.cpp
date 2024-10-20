/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNestedDeclarations.h"
#include <LibWeb/Bindings/CSSNestedDeclarationsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSNestedDeclarations);

JS::NonnullGCPtr<CSSNestedDeclarations> CSSNestedDeclarations::create(JS::Realm& realm, PropertyOwningCSSStyleDeclaration& declaration)
{
    return realm.heap().allocate<CSSNestedDeclarations>(realm, realm, declaration);
}

CSSNestedDeclarations::CSSNestedDeclarations(JS::Realm& realm, PropertyOwningCSSStyleDeclaration& declaration)
    : CSSRule(realm)
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
}

CSSStyleDeclaration* CSSNestedDeclarations::style()
{
    return m_declaration;
}

String CSSNestedDeclarations::serialized() const
{
    // NOTE: There's no proper spec for this yet, only this note:
    // "The CSSNestedDeclarations rule serializes as if its declaration block had been serialized directly."
    // - https://drafts.csswg.org/css-nesting-1/#ref-for-cssnesteddeclarations%E2%91%A1
    // So, we'll do the simple thing and hope it's good.
    return m_declaration->serialized();
}

}
