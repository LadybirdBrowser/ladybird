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
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSKeyframesRule);

GC::Ref<CSSKeyframesRule> CSSKeyframesRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSKeyframesRule>(move(rule));
}

CSSKeyframesRule::CSSKeyframesRule(RustRule rule)
    : CSSRule(move(rule))
{
    auto name = native_rule().payload().name;
    m_name = Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(name.utf16), name.length });
}

GC::Ref<CSSRuleList> CSSKeyframesRule::css_rules() const
{
    if (!m_rules) {
        RustRuleList rules { Parser::ValueParserFFI::rust_rule_list_retain(Parser::ValueParserFFI::rust_rule_children(native_rule().handle())) };
        m_rules = CSSRuleList::create(move(rules), nullptr);
        m_rules->set_owner_rule(*const_cast<CSSKeyframesRule*>(this));
        m_rules->set_parent_style_sheet(const_cast<CSSKeyframesRule*>(this)->parent_style_sheet());
    }
    return *m_rules;
}

void CSSKeyframesRule::set_parent_style_sheet(StyleSheetState* parent_style_sheet)
{
    CSSRule::set_parent_style_sheet(parent_style_sheet);
    if (m_rules) {
        m_rules->set_parent_style_sheet(parent_style_sheet);
    }
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
    if (!is_valid_animation_name_custom_ident(m_name))
        serialize_a_string(builder, m_name);
    else
        serialize_an_identifier(builder, m_name);

    builder.append_ascii(" { "sv);
    for (auto const& keyframe : *css_rules()) {
        builder.append(keyframe->serialized());
        builder.append_ascii(' ');
    }
    builder.append_ascii('}');
    return builder.to_string();
}

WebIDL::UnsignedLong CSSKeyframesRule::length() const
{
    return Parser::ValueParserFFI::rust_rule_list_count(Parser::ValueParserFFI::rust_rule_children(native_rule().handle()));
}

GC::Ptr<CSSKeyframeRule> CSSKeyframesRule::item(size_t index) const
{
    if (index >= length())
        return nullptr;
    return as_if<CSSKeyframeRule>(css_rules()->item(index));
}

void CSSKeyframesRule::set_name(Utf16String const& name)
{
    Utf16FlyString new_name { name };
    if (new_name == m_name)
        return;

    record_style_rule_removed(*this);
    m_name = move(new_name);
    Parser::ValueParserFFI::rust_keyframes_set_name(native_rule().handle(), Parser::ffi_utf16_view(name));
    record_style_rule_inserted(*this);

    if (auto* sheet = parent_style_sheet())
        sheet->invalidate_owners();
}

// https://drafts.csswg.org/css-animations/#interface-csskeyframesrule-appendrule
void CSSKeyframesRule::append_rule(Utf16String const& rule)
{
    // The appendRule method appends the passed CSSKeyframeRule at the end of the keyframes rule.
    auto parsed_rule = Parser::parse_keyframe_rule(Parser::ParsingParams {}, rule);

    if (!parsed_rule.has_value())
        return;

    RustRuleList rules { Parser::ValueParserFFI::rust_rule_list_retain(Parser::ValueParserFFI::rust_rule_children(native_rule().handle())) };
    rules.insert(rules.size(), *parsed_rule);

    if (auto* sheet = parent_style_sheet()) {
        record_style_rule_declarations_changed(*this);
        sheet->invalidate_owners();
    }
}

// https://drafts.csswg.org/css-animations-1/#interface-csskeyframesrule-deleterule
void CSSKeyframesRule::delete_rule(Utf16String const& select)
{
    // The deleteRule method deletes the last declared CSSKeyframeRule matching the specified keyframe selector. If no
    // matching rule exists, the method does nothing.
    auto index = Parser::ValueParserFFI::rust_keyframes_find_rule(native_rule().handle(), Parser::ffi_utf16_view(select));
    if (index == NumericLimits<size_t>::max())
        return;

    if (m_rules)
        MUST(m_rules->remove_a_css_rule(index));
    else
        Parser::ValueParserFFI::rust_rule_list_remove(Parser::ValueParserFFI::rust_rule_children(native_rule().handle()), index);

    if (auto* sheet = parent_style_sheet()) {
        record_style_rule_declarations_changed(*this);
        sheet->invalidate_owners();
    }
}

// https://drafts.csswg.org/css-animations-1/#interface-csskeyframesrule-findrule
GC::Ptr<CSSKeyframeRule> CSSKeyframesRule::find_rule(Utf16String const& select)
{
    // The findRule returns the last declared CSSKeyframeRule matching the specified keyframe selector. If no matching
    // rule exists, the method does nothing.
    auto index = Parser::ValueParserFFI::rust_keyframes_find_rule(native_rule().handle(), Parser::ffi_utf16_view(select));
    return index == NumericLimits<size_t>::max() ? nullptr : item(index);
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
