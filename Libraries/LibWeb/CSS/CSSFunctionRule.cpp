/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionRule.h"
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Bindings/CSSFunctionRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFunctionRule);

static Utf16View parameter_name(Parser::ValueParserFFI::FfiFunctionParameterView const& parameter)
{
    return { reinterpret_cast<char16_t const*>(parameter.name.utf16), parameter.name.length };
}

static RefPtr<StyleValue const> parameter_default_value(Parser::ValueParserFFI::FfiFunctionParameterView const& parameter)
{
    if (!parameter.default_value)
        return {};
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(parameter.default_value)));
}

// https://drafts.csswg.org/css-mixins-1/#dictdef-functionparameter
FunctionParameter FunctionParameter::from_native_parameter(Parser::ValueParserFFI::FfiFunctionParameterView const& parameter)
{
    auto type = Parser::RustSyntaxHandle { Parser::ValueParserFFI::rust_syntax_retain(parameter.syntax) };
    auto default_value = parameter_default_value(parameter);
    return {
        // name
        // The name of the function parameter.
        Utf16FlyString::from_utf16(parameter_name(parameter)),

        // type
        // The type of the function parameter, represented as a syntax string, or "*" if the parameter has no type.
        type.serialize(),

        // defaultValue
        // The default value of the function parameter, or `null` if the argument does not have a default.
        default_value
            ? default_value->to_utf16_string(SerializationMode::Normal)
            : Optional<Utf16String> {},
    };
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-css-type
static void serialize_a_css_type(Utf16StringBuilder& builder, Parser::RustSyntaxHandle const& type)
{
    // To serialize a CSS type, return the concatenation of the following:

    // If the <css-type> consists of a single <syntax-component>, return the corresponding syntax string.
    if (type.is_single_component()) {
        builder.append(type.serialize());
        return;
    }

    // Otherwise, return the concatenation of the following:
    // The string "type(", i.e. "type" followed by a single LEFT PARENTHESIS (U+0028).
    builder.append_ascii("type("sv);

    // The corresponding syntax string.
    builder.append(type.serialize());

    // The string ")", i.e. a single RIGHT PARENTHESIS (U+0029).
    builder.append_ascii(')');
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-function-parameter
static void serialize_function_parameter(Utf16StringBuilder& builder, Parser::ValueParserFFI::FfiFunctionParameterView const& parameter)
{
    auto type = Parser::RustSyntaxHandle { Parser::ValueParserFFI::rust_syntax_retain(parameter.syntax) };
    // To serialize a function parameter, return the concatenation of the following:

    // The result of performing serialize an identifier on the name of the function parameter.
    serialize_an_identifier(builder, parameter_name(parameter));

    // If the function parameter has a type, and that type is not the universal syntax definition:
    if (!type.is_universal()) {
        // - A single SPACE (U+0020), followed by the result of performing serialize a CSS type on that type.
        builder.append_ascii(' ');
        serialize_a_css_type(builder, type);
    }

    // If the function parameter has a default value:
    if (auto default_value = parameter_default_value(parameter)) {
        // - A single COLON (U+003A), followed by a single SPACE (U+0020), followed by the result of performing
        //   serialize a CSS value on that value.
        builder.append_ascii(": "sv);
        default_value->serialize(builder, SerializationMode::Normal);
    }
}

GC::Ref<CSSFunctionRule> CSSFunctionRule::create(RustRule rule, CSSRuleList& rules)
{
    return GC::Heap::the().allocate<CSSFunctionRule>(move(rule), rules);
}

CSSFunctionRule::CSSFunctionRule(RustRule rule, CSSRuleList& rules)
    : CSSGroupingRule(rules, move(rule))
    , m_signature(*native_rule().payload().function_signature)
{
}

Utf16String CSSFunctionRule::name() const
{
    auto name = Parser::ValueParserFFI::rust_function_signature_view(&m_signature).name;
    return Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(name.utf16), name.length });
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-getparameters
Vector<FunctionParameter> CSSFunctionRule::get_parameters() const
{
    Vector<FunctionParameter> parameters;
    auto count = Parser::ValueParserFFI::rust_function_signature_view(&m_signature).parameter_count;
    parameters.ensure_capacity(count);

    for (size_t index = 0; index < count; ++index)
        parameters.append(FunctionParameter::from_native_parameter(Parser::ValueParserFFI::rust_function_signature_parameter(&m_signature, index)));

    return parameters;
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-returntype
Utf16String CSSFunctionRule::return_type() const
{
    // The return type of the custom function, represented as a syntax string. If the custom function has no return
    // type, returns "*".
    // NB: We always store a return type (defaulting to "*")
    return Parser::RustSyntaxHandle { Parser::ValueParserFFI::rust_syntax_retain(Parser::ValueParserFFI::rust_function_signature_view(&m_signature).return_type) }.serialize();
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-cssfunctionrule
Utf16String CSSFunctionRule::serialized() const
{
    auto signature = Parser::ValueParserFFI::rust_function_signature_view(&m_signature);
    // To serialize a CSSFunctionRule, return the concatenation of the following:
    Utf16StringBuilder builder;

    // 1. The string "@function" followed by a single SPACE (U+0020).
    builder.append_ascii("@function "sv);

    // 2. The result of performing serialize an identifier on the name of the custom function, followed by a single LEFT
    //    PARENTHESIS (U+0028).
    serialize_an_identifier(builder, { reinterpret_cast<char16_t const*>(signature.name.utf16), signature.name.length });
    builder.append_ascii('(');

    // 3. The result of serialize a function parameter on each of the custom function’s parameters, all joined by ", "
    //    (COMMA U+002C, followed by a single SPACE U+0020).
    for (size_t i = 0; i < signature.parameter_count; ++i) {
        if (i > 0)
            builder.append_ascii(", "sv);
        serialize_function_parameter(builder, Parser::ValueParserFFI::rust_function_signature_parameter(&m_signature, i));
    }

    // 4. A single RIGHT PARENTHESIS (U+0029).
    builder.append_ascii(')');

    // 5. If the custom function has return type, and that return type is not the universal syntax definition ("*"):
    if (auto return_type = Parser::RustSyntaxHandle { Parser::ValueParserFFI::rust_syntax_retain(signature.return_type) }; !return_type.is_universal()) {
        // - A single SPACE (U+0020), followed by the string "returns", followed by a single SPACE (U+0020).
        builder.append_ascii(" returns "sv);

        // - The result of performing serialize a CSS type on that type.
        serialize_a_css_type(builder, return_type);
    }

    // 6. A single SPACE (U+0020), followed by a LEFT CURLY BRACKET (U+007B).
    builder.append_ascii(" {"sv);

    // 7. The result of performing serialize a CSS rule on each rule in cssRules, filtering out empty strings, each
    //    preceded by a single SPACE (U+0020).
    auto const& rules = css_rules();

    for (size_t i = 0; i < rules.length(); ++i) {
        auto const& rule = rules.item(i);

        auto serialized_rule = rule->serialized();

        if (!serialized_rule.is_empty()) {
            builder.append_ascii(' ');
            builder.append(serialized_rule);
        }
    }

    // 8. A single SPACE (U+0020), followed by a single RIGHT CURLY BRACKET (U+007D).
    builder.append_ascii(" }"sv);

    return builder.to_string();
}

}
