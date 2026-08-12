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
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSKeyframesRule);

GC::Ref<CSSKeyframesRule> CSSKeyframesRule::create(Utf16FlyString name, GC::Ref<CSSRuleList> css_rules)
{
    return GC::Heap::the().allocate<CSSKeyframesRule>(move(name), move(css_rules));
}

CSSKeyframesRule::CSSKeyframesRule(Utf16FlyString name, GC::Ref<CSSRuleList> keyframes)
    : CSSRule(Type::Keyframes)
    , m_name(move(name))
    , m_rules(move(keyframes))
{
    for (auto& rule : *m_rules)
        rule->set_parent_rule(this);
}

void CSSKeyframesRule::set_parent_style_sheet(CSSStyleSheet* parent_style_sheet)
{
    CSSRule::set_parent_style_sheet(parent_style_sheet);
    for (auto& rule : *m_rules)
        rule->set_parent_style_sheet(parent_style_sheet);
}

void CSSKeyframesRule::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rules);
}

Utf16String CSSKeyframesRule::serialized() const
{
    Utf16StringBuilder builder;

    builder.append("@keyframes "sv);

    // https://drafts.csswg.org/css-animations-1/#keyframes
    // When serialized, the value is serialized as an <ident> unless it’s a disallowed keyword, in which case
    // it’s serialized as a <string>.
    if (!is_valid_custom_ident(m_name, { { "none"sv } }))
        serialize_a_string(builder, m_name);
    else
        serialize_an_identifier(builder, m_name);

    builder.append_ascii(" { "sv);
    for (auto const& keyframe : *m_rules) {
        builder.append(keyframe->serialized());
        builder.append_ascii(' ');
    }
    builder.append_ascii('}');
    return builder.to_string();
}

WebIDL::UnsignedLong CSSKeyframesRule::length() const
{
    return m_rules->length();
}

GC::Ptr<CSSKeyframeRule> CSSKeyframesRule::item(size_t index) const
{
    return as_if<CSSKeyframeRule>(m_rules->item(index));
}

void CSSKeyframesRule::set_name(Utf16String const& name)
{
    Utf16FlyString new_name { name };
    if (new_name == m_name)
        return;

    record_style_rule_removed(*this);
    m_name = move(new_name);
    record_style_rule_inserted(*this);

    if (auto* sheet = parent_style_sheet())
        invalidate_rule_cache_for_style_sheet_owners(*sheet);
}

// https://drafts.csswg.org/css-animations/#interface-csskeyframesrule-appendrule
void CSSKeyframesRule::append_rule(Utf16String const& rule)
{
    // The appendRule method appends the passed CSSKeyframeRule at the end of the keyframes rule.
    auto parsed_rule = Parser::parse_keyframe_rule(Parser::ParsingParams {}, rule);

    if (!parsed_rule)
        return;

    // AD-HOC: The spec doesn't say where to set the parent rule, so we'll do it here.
    parsed_rule->set_parent_rule(this);

    // NB: this only returns an exception if the rule is invalid or the index is out of bounds, neither of which are
    //     applicable here.
    MUST(m_rules->insert_a_css_rule(parsed_rule.ptr(), m_rules->length(), CSSRuleList::Nested::Yes, {}));

    if (auto* sheet = parent_style_sheet()) {
        record_style_rule_declarations_changed(*this);
        invalidate_rule_cache_for_style_sheet_owners(*sheet);
    }
}

// https://drafts.csswg.org/css-animations-1/#interface-csskeyframesrule-deleterule
void CSSKeyframesRule::delete_rule(Utf16String const& select)
{
    // The deleteRule method deletes the last declared CSSKeyframeRule matching the specified keyframe selector. If no
    // matching rule exists, the method does nothing.
    auto selectors = Parser::parse_keyframe_selectors(Parser::ParsingParams {}, select);

    if (selectors.is_empty())
        return;

    for (size_t i = m_rules->length(); i-- > 0;) {
        auto const& keyframe_rule = as<CSSKeyframeRule>(*m_rules->item(i));

        if (keyframe_rule.keys() == selectors) {
            MUST(m_rules->remove_a_css_rule(i));

            if (auto* sheet = parent_style_sheet()) {
                record_style_rule_declarations_changed(*this);
                invalidate_rule_cache_for_style_sheet_owners(*sheet);
            }

            return;
        }
    }
}

// https://drafts.csswg.org/css-animations-1/#interface-csskeyframesrule-findrule
GC::Ptr<CSSKeyframeRule> CSSKeyframesRule::find_rule(Utf16String const& select)
{
    // The findRule returns the last declared CSSKeyframeRule matching the specified keyframe selector. If no matching
    // rule exists, the method does nothing.
    auto selectors = Parser::parse_keyframe_selectors(Parser::ParsingParams {}, select);

    if (selectors.is_empty())
        return nullptr;

    for (size_t i = m_rules->length(); i-- > 0;) {
        auto& keyframe_rule = as<CSSKeyframeRule>(*m_rules->item(i));

        if (keyframe_rule.keys() == selectors)
            return keyframe_rule;
    }

    return nullptr;
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
