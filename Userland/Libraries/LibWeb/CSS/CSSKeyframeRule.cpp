/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSKeyframeRule.h"
#include <LibWeb/Bindings/CSSKeyframeRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSRuleList.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSKeyframeRule);

JS::NonnullGCPtr<CSSKeyframeRule> CSSKeyframeRule::create(JS::Realm& realm, CSS::Percentage key, Web::CSS::PropertyOwningCSSStyleDeclaration& declarations)
{
    return realm.heap().allocate<CSSKeyframeRule>(realm, realm, key, declarations);
}

CSSKeyframeRule::CSSKeyframeRule(JS::Realm& realm, CSS::Percentage key, PropertyOwningCSSStyleDeclaration& declarations)
    : CSSRule(realm)
    , m_key(key)
    , m_declarations(declarations)
{
    m_declarations->set_parent_rule(*this);
}

void CSSKeyframeRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_declarations);
}

void CSSKeyframeRule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSKeyframeRule);
}

String CSSKeyframeRule::serialized() const
{
    StringBuilder builder;
    builder.appendff("{}% {{ {} }}", key().value(), style()->serialized());
    return MUST(builder.to_string());
}

}
