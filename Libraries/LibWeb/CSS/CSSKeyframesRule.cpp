/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetState.h>

namespace Web::CSS {

using Parser::ValueParserFFI::NativeRule;

// NB: What the implementation of CSSKeyframesRule in Rust asks of the C++ code of LibWeb.
extern "C" {
void web_css_keyframes_rule_changed(void* rule);
void web_css_keyframes_rule_will_rename(void* rule);
void web_css_keyframes_rule_renamed(void* rule);
NativeRule const* web_css_parse_keyframe_rule(FlatPtr text);
}

static CSSRule& rule_from_cell(void* cell)
{
    return *static_cast<CSSRule*>(cell);
}

void web_css_keyframes_rule_changed(void* cell)
{
    auto& rule = rule_from_cell(cell);
    record_style_rule_declarations_changed(rule);
    rule.parent_style_sheet()->invalidate_owners();
}

void web_css_keyframes_rule_will_rename(void* cell)
{
    record_style_rule_removed(rule_from_cell(cell));
}

void web_css_keyframes_rule_renamed(void* cell)
{
    auto& rule = rule_from_cell(cell);
    record_style_rule_inserted(rule);
    if (auto* sheet = rule.parent_style_sheet())
        sheet->invalidate_owners();
}

NativeRule const* web_css_parse_keyframe_rule(FlatPtr text)
{
    auto parsed_rule = Parser::parse_keyframe_rule(Parser::ParsingParams {}, Utf16String::adopt_raw(text));
    if (!parsed_rule.has_value())
        return nullptr;
    return Parser::ValueParserFFI::rust_rule_retain(parsed_rule->handle());
}

}
