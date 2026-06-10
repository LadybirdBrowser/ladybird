/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSKeyframesRule);

GC::Ref<CSSKeyframesRule> CSSKeyframesRule::create(FlyString name, GC::Ref<CSSRuleList> css_rules)
{
    return GC::Heap::the().allocate<CSSKeyframesRule>(move(name), move(css_rules));
}

CSSKeyframesRule::CSSKeyframesRule(FlyString name, GC::Ref<CSSRuleList> keyframes)
    : CSSRule(Type::Keyframes)
    , m_name(move(name))
    , m_rules(move(keyframes))
{
    for (auto& rule : *m_rules)
        rule->set_parent_rule(this);
}

void CSSKeyframesRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rules);
}

String CSSKeyframesRule::serialized() const
{
    StringBuilder builder;
    builder.appendff("@keyframes \"{}\"", name());
    builder.append(" { "sv);
    for (auto const& keyframe : *m_rules) {
        builder.append(keyframe->css_text());
        builder.append(' ');
    }
    builder.append('}');
    return MUST(builder.to_string());
}

WebIDL::UnsignedLong CSSKeyframesRule::length() const
{
    return m_rules->length();
}

void CSSKeyframesRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Name: {}\n", name());
    dump_indent(builder, indent_levels + 1);
    builder.appendff("Keyframes ({}):\n", length());
    for (auto& rule : *css_rules())
        dump_rule(builder, rule, indent_levels + 2);
}

}
