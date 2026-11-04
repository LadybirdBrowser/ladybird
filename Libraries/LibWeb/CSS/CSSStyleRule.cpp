/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetState.h>

namespace Web::CSS {

// NB: Implemented in Libraries/LibWeb/Bindings/Rust/src/css/exports.rs.
extern "C" void web_css_style_rule_set_selector_text(void* rule, FlatPtr text);

// NB: What the implementation of CSSStyleRule in Rust asks of the C++ code of LibWeb.
extern "C" void web_css_style_rule_selector_changed(void* rule);

SelectorList CSSStyleRule::selectors() const
{
    return selector_list_from_rust(static_cast<SelectorFFI::RustParsedSelectorList const*>(Parser::ValueParserFFI::rust_rule_selectors(native_rule().handle())));
}

SelectorList CSSStyleRule::absolutized_selectors() const
{
    return matching_selectors_for_rule(native_rule());
}

RustDeclarationBlock CSSStyleRule::declaration() const
{
    return RustDeclarationBlock { Parser::ValueParserFFI::rust_rule_declarations(native_rule().handle()) };
}

// https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
Utf16String CSSStyleRule::selector_text() const
{
    // The selectorText attribute, on getting, must return the result of serializing the associated group of selectors.
    return serialize_a_group_of_selectors(selectors(), parent_style_sheet());
}

void CSSStyleRule::set_selector_text(Utf16View selector_text)
{
    web_css_style_rule_set_selector_text(this, Utf16String::from_utf16(selector_text).into_raw());
}

GC::Ref<CSSStyleProperties> CSSStyleRule::style() const
{
    return static_cast<CSSStyleProperties&>(*declarations());
}

void web_css_style_rule_selector_changed(void* cell)
{
    auto& rule = *static_cast<CSSStyleRule*>(cell);
    record_style_rule_selector_changed(rule);
    rule.parent_style_sheet()->invalidate_owners();
}

}
