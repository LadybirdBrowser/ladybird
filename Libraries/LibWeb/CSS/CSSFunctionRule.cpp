/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionRule.h"
#include <LibWeb/Bindings/CSSFunctionRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFunctionRule);

// https://drafts.csswg.org/css-mixins-1/#dictdef-functionparameter
FunctionParameter FunctionParameter::from_internal_function_parameter(FunctionParameterInternal const& internal)
{
    return {
        // name
        // The name of the function parameter.
        internal.name,

        // type
        // The type of the function parameter, represented as a syntax string, or "*" if the parameter has no type.
        internal.type->to_string(),

        // defaultValue
        // The default value of the function parameter, or `null` if the argument does not have a default.
        internal.default_value.has_value() ? MUST(serialize_a_series_of_component_values(internal.default_value.value()).trim_ascii_whitespace()) : Optional<String> {},
    };
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-css-type
static void serialize_a_css_type(StringBuilder& builder, Parser::SyntaxNode const& type)
{
    // To serialize a CSS type, return the concatenation of the following:

    // If the <css-type> consists of a single <syntax-component>, return the corresponding syntax string.
    auto const is_single_syntax_component = [&]() {
        if (type.type() == Parser::SyntaxNode::NodeType::Universal)
            return true;

        if (first_is_one_of(type.type(), Parser::SyntaxNode::NodeType::Ident, Parser::SyntaxNode::NodeType::Type))
            return true;

        if (first_is_one_of(type.type(), Parser::SyntaxNode::NodeType::Multiplier, Parser::SyntaxNode::NodeType::CommaSeparatedMultiplier) && first_is_one_of(as<Parser::MultiplierSyntaxNode>(type).child().type(), Parser::SyntaxNode::NodeType::Ident, Parser::SyntaxNode::NodeType::Type))
            return true;

        return false;
    }();

    if (is_single_syntax_component) {
        builder.append(type.to_string());
        return;
    }

    // Otherwise, return the concatenation of the following:
    // The string "type(", i.e. "type" followed by a single LEFT PARENTHESIS (U+0028).
    builder.append("type("sv);

    // The corresponding syntax string.
    builder.append(type.to_string());

    // The string ")", i.e. a single RIGHT PARENTHESIS (U+0029).
    builder.append(')');
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-function-parameter
void FunctionParameterInternal::serialize(StringBuilder& builder) const
{
    // To serialize a function parameter, return the concatenation of the following:

    // The result of performing serialize an identifier on the name of the function parameter.
    serialize_an_identifier(builder, name);

    // If the function parameter has a type, and that type is not the universal syntax definition:
    if (type->type() != Parser::SyntaxNode::NodeType::Universal) {
        // - A single SPACE (U+0020), followed by the result of performing serialize a CSS type on that type.
        builder.append(' ');
        serialize_a_css_type(builder, *type);
    }

    // If the function parameter has a default value:
    if (default_value.has_value()) {
        // - A single COLON (U+003A), followed by a single SPACE (U+0020), followed by the result of performing
        //   serialize a CSS value on that value.
        builder.appendff(": {}", MUST(serialize_a_series_of_component_values(default_value.value()).trim_ascii_whitespace()));
    }
}

GC::Ref<CSSFunctionRule> CSSFunctionRule::create(JS::Realm& realm, CSSRuleList& rules, FlyString name, Vector<FunctionParameterInternal> parameters, NonnullOwnPtr<Parser::SyntaxNode> return_type)
{
    return realm.create<CSSFunctionRule>(realm, rules, move(name), move(parameters), move(return_type));
}

CSSFunctionRule::CSSFunctionRule(JS::Realm& realm, CSSRuleList& rules, FlyString name, Vector<FunctionParameterInternal> parameters, NonnullOwnPtr<Parser::SyntaxNode> return_type)
    : CSSGroupingRule(realm, rules, Type::Function)
    , m_name(move(name))
    , m_parameters(move(parameters))
    , m_return_type(move(return_type))
{
}

void CSSFunctionRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFunctionRule);
    Base::initialize(realm);
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-getparameters
Vector<FunctionParameter> CSSFunctionRule::get_parameters() const
{
    Vector<FunctionParameter> parameters;
    parameters.ensure_capacity(m_parameters.size());

    for (auto const& parameter : m_parameters)
        parameters.append(FunctionParameter::from_internal_function_parameter(parameter));

    return parameters;
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctionrule-returntype
String CSSFunctionRule::return_type() const
{
    // The return type of the custom function, represented as a syntax string. If the custom function has no return
    // type, returns "*".
    // NB: We always store a return type (defaulting to "*")
    return m_return_type->to_string();
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-cssfunctionrule
String CSSFunctionRule::serialized() const
{
    // To serialize a CSSFunctionRule, return the concatenation of the following:
    StringBuilder builder;

    // 1. The string "@function" followed by a single SPACE (U+0020).
    builder.append("@function "sv);

    // 2. The result of performing serialize an identifier on the name of the custom function, followed by a single LEFT
    //    PARENTHESIS (U+0028).
    serialize_an_identifier(builder, m_name);
    builder.append('(');

    // 3. The result of serialize a function parameter on each of the custom function’s parameters, all joined by ", "
    //    (COMMA U+002C, followed by a single SPACE U+0020).
    for (size_t i = 0; i < m_parameters.size(); ++i) {
        if (i > 0)
            builder.append(", "sv);
        m_parameters[i].serialize(builder);
    }

    // 4. A single RIGHT PARENTHESIS (U+0029).
    builder.append(')');

    // 5. If the custom function has return type, and that return type is not the universal syntax definition ("*"):
    if (m_return_type->type() != Parser::SyntaxNode::NodeType::Universal) {
        // - A single SPACE (U+0020), followed by the string "returns", followed by a single SPACE (U+0020).
        builder.append(" returns "sv);

        // - The result of performing serialize a CSS type on that type.
        serialize_a_css_type(builder, *m_return_type);
    }

    // 6. A single SPACE (U+0020), followed by a LEFT CURLY BRACKET (U+007B).
    builder.append(" {"sv);

    // 7. The result of performing serialize a CSS rule on each rule in cssRules, filtering out empty strings, each
    //    preceded by a single SPACE (U+0020).
    auto const& rules = css_rules();

    for (size_t i = 0; i < rules.length(); ++i) {
        auto const& rule = rules.item(i);

        auto serialized_rule = rule->serialized();

        if (!serialized_rule.is_empty()) {
            builder.append(' ');
            builder.append(serialized_rule);
        }
    }

    // 8. A single SPACE (U+0020), followed by a single RIGHT CURLY BRACKET (U+007D).
    builder.append(" }"sv);

    return MUST(builder.to_string());
}

}
