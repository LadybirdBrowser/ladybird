/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSKeyframeRule.h"
#include <LibWeb/Bindings/CSSKeyframeRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSKeyframeRule);

GC::Ref<CSSKeyframeRule> CSSKeyframeRule::create(JS::Realm& realm, Percentage key, CSSStyleProperties& declarations)
{
    return realm.create<CSSKeyframeRule>(realm, key, declarations);
}

CSSKeyframeRule::CSSKeyframeRule(JS::Realm& realm, Percentage key, CSSStyleProperties& declarations)
    : CSSRule(realm, Type::Keyframe)
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
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSKeyframeRule);
    Base::initialize(realm);
}

String CSSKeyframeRule::serialized() const
{
    StringBuilder builder;
    builder.appendff("{}% {{ {} }}", key().value(), style()->serialized());
    return MUST(builder.to_string());
}

void CSSKeyframeRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Key: {}\n"sv, key_text());
    dump_style_properties(builder, style(), indent_levels + 1);
}

}
