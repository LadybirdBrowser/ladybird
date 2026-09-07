/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StylePropertyMap.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleRule);

GC::Ref<CSSStyleRule> CSSStyleRule::create(RustRule rule, CSSRuleList& nested_rules)
{
    return GC::Heap::the().allocate<CSSStyleRule>(move(rule), nested_rules);
}

CSSStyleRule::CSSStyleRule(RustRule rule, CSSRuleList& nested_rules)
    : CSSGroupingRule(nested_rules, move(rule))
    , m_declarations(Parser::ValueParserFFI::rust_rule_declarations(native_rule().handle()))
{
}

SelectorList const& CSSStyleRule::selectors() const
{
    if (!m_selectors.has_value())
        m_selectors = selector_list_from_rust(static_cast<SelectorFFI::RustParsedSelectorList const*>(Parser::ValueParserFFI::rust_rule_selectors(native_rule().handle())));
    return *m_selectors;
}

size_t CSSStyleRule::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(Base::external_memory_size(), m_declarations.external_memory_size());
}

GC::Ref<CSSStyleProperties> CSSStyleRule::ensure_style_properties() const
{
    if (!m_declaration) {
        m_declaration = CSSStyleProperties::create(m_declarations.retain());
        m_declaration->set_parent_rule(const_cast<CSSStyleRule&>(*this));
    }
    return *m_declaration;
}

void CSSStyleRule::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_declaration);
    visitor.visit(m_style_map);
}

// https://drafts.csswg.org/cssom-1/#dom-cssstylerule-style
GC::Ref<CSSStyleProperties> CSSStyleRule::style() const
{
    return ensure_style_properties();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylerule-stylemap
GC::Ref<StylePropertyMap> CSSStyleRule::style_map()
{
    if (!m_style_map)
        m_style_map = StylePropertyMap::create(ensure_style_properties());
    return *m_style_map;
}

// https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
Utf16String CSSStyleRule::serialized() const
{
    Utf16StringBuilder builder;

    // 1. Let s initially be the result of performing serialize a group of selectors on the rule’s associated selectors,
    //    followed by the string " {", i.e., a single SPACE (U+0020), followed by LEFT CURLY BRACKET (U+007B).
    builder.append(selector_text());
    builder.append_ascii(" {"sv);

    // 2. Let decls be the result of performing serialize a CSS declaration block on the rule’s associated declarations,
    //    or null if there are no such declarations.
    auto decls = !m_declarations.is_empty() ? Optional<Utf16String> { ensure_style_properties()->serialized() } : Optional<Utf16String> {};

    // 3. Let rules be the result of performing serialize a CSS rule on each rule in the rule’s cssRules list,
    //    or null if there are no such rules.
    Vector<Utf16String> rules;
    for (auto& rule : css_rules()) {
        rules.append(rule->serialized());
    }

    // 4. If decls and rules are both null, append " }" to s (i.e. a single SPACE (U+0020) followed by RIGHT CURLY BRACKET (U+007D)) and return s.
    if (!decls.has_value() && rules.is_empty()) {
        builder.append_ascii(" }"sv);
        return builder.to_string();
    }

    // 5. If rules is null:
    if (rules.is_empty()) {
        // 1. Append a single SPACE (U+0020) to s
        builder.append_ascii(' ');
        // 2. Append decls to s
        builder.append(*decls);
        // 3. Append " }" to s (i.e. a single SPACE (U+0020) followed by RIGHT CURLY BRACKET (U+007D)).
        builder.append_ascii(" }"sv);
        // 4. Return s.
        return builder.to_string();
    }

    // 6. Otherwise:
    else {
        // 1. If decls is not null, prepend it to rules.
        if (decls.has_value())
            rules.prepend(decls.value());

        // 2. For each rule in rules:
        for (auto& rule : rules) {
            // * If rule is the empty string, do nothing.
            if (rule.is_empty())
                continue;

            // * Otherwise:
            // 1. Append a newline followed by two spaces to s.
            // 2. Append rule to s.
            builder.appendff("\n  {}", rule);
        }

        // 3. Append a newline followed by RIGHT CURLY BRACKET (U+007D) to s.
        builder.append_ascii("\n}"sv);

        // 4. Return s.
        return builder.to_string();
    }
}

// https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
Utf16String CSSStyleRule::selector_text() const
{
    // The selectorText attribute, on getting, must return the result of serializing the associated group of selectors.
    return serialize_a_group_of_selectors(selectors(), parent_style_sheet());
}

// https://drafts.csswg.org/cssom-1/#dom-cssstylerule-selectortext
void CSSStyleRule::set_selector_text(Utf16View selector_text)
{
    // 1. Run the parse a group of selectors algorithm on the given value.
    auto* sheet = parent_style_sheet();

    // 2. If the algorithm returns a non-null value replace the associated group of selectors with the returned value.
    if (Parser::ValueParserFFI::rust_rule_set_selector_text(native_rule().handle(), Parser::ffi_utf16_view(selector_text), sheet ? sheet->native_rules().handle() : nullptr)) {
        m_selectors.clear();
        clear_caches();
        if (sheet) {
            record_style_rule_selector_changed(*this);
            sheet->invalidate_owners();
        }
    }

    // 3. Otherwise, if the algorithm returns a null value, do nothing.
}

SelectorList const& CSSStyleRule::absolutized_selectors() const
{
    if (m_cached_absolutized_selectors.has_value())
        return m_cached_absolutized_selectors.value();

    m_cached_absolutized_selectors = matching_selectors_for_rule(native_rule());
    return m_cached_absolutized_selectors.value();
}

void CSSStyleRule::clear_caches()
{
    Base::clear_caches();
    m_cached_absolutized_selectors.clear();
}

void CSSStyleRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    for (auto& selector : selectors()) {
        dump_selector(builder, selector, indent_levels + 1);
    }
    dump_indent(builder, indent_levels + 1);
    builder.appendff("Absolutized selectors:\n");
    for (auto& selector : absolutized_selectors()) {
        dump_selector(builder, selector, indent_levels + 2);
    }
    dump_style_properties(builder, *ensure_style_properties(), indent_levels + 1);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Child rules ({}):\n", css_rules().length());
    for (auto& child_rule : css_rules())
        dump_rule(builder, child_rule, indent_levels + 2);
}

}
