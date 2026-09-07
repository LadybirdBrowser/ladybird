/*
 * Copyright (c) 2024, Alex Ungurianu <alex@ungurianu.com>
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/Parser/RustSyntaxHandle.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPropertyRule);

GC::Ref<CSSPropertyRule> CSSPropertyRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSPropertyRule>(move(rule));
}

CSSPropertyRule::CSSPropertyRule(RustRule rule)
    : CSSRule(move(rule))
    , m_rule(*native_rule().payload().property)
{
}

CSSPropertyRule::~CSSPropertyRule() = default;

Utf16View CSSPropertyRule::name() const
{
    auto name = Parser::ValueParserFFI::rust_property_rule_view(&m_rule).name;
    return { reinterpret_cast<char16_t const*>(name.utf16), name.length };
}

Utf16View CSSPropertyRule::syntax() const
{
    auto syntax = Parser::ValueParserFFI::rust_property_rule_view(&m_rule).syntax_source;
    return { reinterpret_cast<char16_t const*>(syntax.utf16), syntax.length };
}

bool CSSPropertyRule::inherits() const
{
    return Parser::ValueParserFFI::rust_property_rule_view(&m_rule).inherits;
}

RefPtr<StyleValue const> CSSPropertyRule::initial_style_value() const
{
    auto* value = Parser::ValueParserFFI::rust_property_rule_view(&m_rule).initial_value;
    if (!value)
        return {};
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(value)));
}

Optional<Utf16String> CSSPropertyRule::initial_value() const
{
    if (auto initial_value = initial_style_value())
        return initial_value->to_utf16_string(SerializationMode::Normal);
    return {};
}

// https://www.w3.org/TR/cssom-1/#serialize-a-css-rule
Utf16String CSSPropertyRule::serialized() const
{
    Utf16StringBuilder builder;

    // Serialization algorithm is defined in the spec below
    // https://drafts.css-houdini.org/css-properties-values-api/#the-css-property-rule-interface

    // To serialize a CSSPropertyRule, return the concatenation of the following:

    // 1. The string "@property" followed by a single SPACE (U+0020).
    // 2. The result of performing serialize an identifier on the rule’s name, followed by a single SPACE (U+0020).
    builder.append_ascii("@property "sv);
    serialize_an_identifier(builder, name());
    builder.append_ascii(' ');

    // 3. The string "{ ", i.e., a single LEFT CURLY BRACKET (U+007B), followed by a SPACE (U+0020).
    builder.append_ascii("{ "sv);

    // 4. The string "syntax:", followed by a single SPACE (U+0020).
    // 5. The result of performing serialize a string on the rule’s syntax, followed by a single SEMICOLON (U+003B), followed by a SPACE (U+0020).
    builder.append_ascii("syntax: "sv);
    serialize_a_string(builder, syntax());
    builder.append_ascii("; "sv);

    // 6. The string "inherits:", followed by a single SPACE (U+0020).
    // 7. For the rule’s inherits attribute, one of the following depending on the attribute’s value:
    //      true:  The string "true" followed by a single SEMICOLON (U+003B), followed by a SPACE (U+0020).
    //      false: The string "false" followed by a single SEMICOLON (U+003B), followed by a SPACE (U+0020).
    builder.appendff("inherits: {}; ", inherits());

    // 8. If the rule’s initial-value is present, follow these substeps:
    if (auto initial_value = initial_style_value()) {
        // 1. The string "initial-value:".
        // 2. The result of performing serialize a CSS value in the rule’s initial-value followed by a single SEMICOLON
        //    (U+003B), followed by a SPACE (U+0020).
        builder.append_ascii("initial-value: "sv);
        initial_value->serialize(builder, SerializationMode::Normal);
        builder.append_ascii("; "sv);
    }
    // 9. A single RIGHT CURLY BRACKET (U+007D).
    builder.append_ascii("}"sv);

    return builder.to_string();
}

void CSSPropertyRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Name: {}\n", name());

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Syntax: `{}`\n", syntax());
    dump_indent(builder, indent_levels + 2);
    builder.appendff("Parsed syntax: {}\n", Parser::RustSyntaxHandle { Parser::ValueParserFFI::rust_syntax_retain(Parser::ValueParserFFI::rust_property_rule_view(&m_rule).syntax) }.serialize());

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Inherits: {}\n", inherits());

    if (auto initial_value = initial_style_value()) {
        dump_indent(builder, indent_levels + 1);
        builder.append("Initial value: "sv);
        initial_value->serialize(builder, SerializationMode::Normal);
        builder.append('\n');
    }
}

}
