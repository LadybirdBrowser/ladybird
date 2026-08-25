/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionRule.h"
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Bindings/CSSFunctionRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSConditionRule.h>
#include <LibWeb/CSS/CSSContainerRule.h>
#include <LibWeb/CSS/CSSFunctionDeclarations.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Document.h>

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
        internal.type.serialize(),

        // defaultValue
        // The default value of the function parameter, or `null` if the argument does not have a default.
        internal.default_value
            ? internal.default_value->to_utf16_string(SerializationMode::Normal)
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
void FunctionParameterInternal::serialize(Utf16StringBuilder& builder) const
{
    // To serialize a function parameter, return the concatenation of the following:

    // The result of performing serialize an identifier on the name of the function parameter.
    serialize_an_identifier(builder, name);

    // If the function parameter has a type, and that type is not the universal syntax definition:
    if (!type.is_universal()) {
        // - A single SPACE (U+0020), followed by the result of performing serialize a CSS type on that type.
        builder.append_ascii(' ');
        serialize_a_css_type(builder, type);
    }

    // If the function parameter has a default value:
    if (default_value) {
        // - A single COLON (U+003A), followed by a single SPACE (U+0020), followed by the result of performing
        //   serialize a CSS value on that value.
        builder.appendff(": {}", default_value->to_string(SerializationMode::Normal));
    }
}

GC::Ref<CSSFunctionRule> CSSFunctionRule::create(CSSRuleList& rules, Utf16FlyString name, Vector<FunctionParameterInternal> parameters, Parser::RustSyntaxHandle return_type)
{
    return GC::Heap::the().allocate<CSSFunctionRule>(rules, move(name), move(parameters), move(return_type));
}

CSSFunctionRule::CSSFunctionRule(CSSRuleList& rules, Utf16FlyString name, Vector<FunctionParameterInternal> parameters, Parser::RustSyntaxHandle return_type)
    : CSSGroupingRule(rules, Type::Function)
    , m_name(move(name))
    , m_parameters(move(parameters))
    , m_return_type(move(return_type))
{
}

Utf16String CSSFunctionRule::name() const
{
    return m_name.to_utf16_string();
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
Utf16String CSSFunctionRule::return_type() const
{
    // The return type of the custom function, represented as a syntax string. If the custom function has no return
    // type, returns "*".
    // NB: We always store a return type (defaulting to "*")
    return m_return_type.serialize();
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-cssfunctionrule
Utf16String CSSFunctionRule::serialized() const
{
    // To serialize a CSSFunctionRule, return the concatenation of the following:
    Utf16StringBuilder builder;

    // 1. The string "@function" followed by a single SPACE (U+0020).
    builder.append_ascii("@function "sv);

    // 2. The result of performing serialize an identifier on the name of the custom function, followed by a single LEFT
    //    PARENTHESIS (U+0028).
    serialize_an_identifier(builder, m_name);
    builder.append_ascii('(');

    // 3. The result of serialize a function parameter on each of the custom function’s parameters, all joined by ", "
    //    (COMMA U+002C, followed by a single SPACE U+0020).
    for (size_t i = 0; i < m_parameters.size(); ++i) {
        if (i > 0)
            builder.append_ascii(", "sv);
        m_parameters[i].serialize(builder);
    }

    // 4. A single RIGHT PARENTHESIS (U+0029).
    builder.append_ascii(')');

    // 5. If the custom function has return type, and that return type is not the universal syntax definition ("*"):
    if (!m_return_type.is_universal()) {
        // - A single SPACE (U+0020), followed by the string "returns", followed by a single SPACE (U+0020).
        builder.append_ascii(" returns "sv);

        // - The result of performing serialize a CSS type on that type.
        serialize_a_css_type(builder, m_return_type);
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

template<typename Callback>
static void for_each_effective_function_declarations_rule(CSSRuleList const& rule_list, GC::Ptr<CSSContainerRule const> container_rule, Callback const& callback)
{
    for (auto const& rule : rule_list) {
        switch (rule->type()) {
        case CSSRule::Type::Container: {
            auto const& nested_container_rule = as<CSSContainerRule>(*rule);
            for_each_effective_function_declarations_rule(nested_container_rule.css_rules(), &nested_container_rule, callback);
            break;
        }
        case CSSRule::Type::Media:
        case CSSRule::Type::Supports: {
            auto const& condition_rule = as<CSSConditionRule>(*rule);
            if (condition_rule.condition_matches())
                for_each_effective_function_declarations_rule(condition_rule.css_rules(), container_rule, callback);
            break;
        }
        case CSSRule::Type::FunctionDeclarations:
            callback(as<CSSFunctionDeclarations>(*rule), container_rule);
            break;
        default:
            break;
        }
    }
}

void CSSFunctionRule::for_each_effective_declaration(DOM::AbstractElement& root_element, Function<void(Utf16FlyString const&, NonnullRefPtr<StyleValue const> const&)> const& callback) const
{
    for_each_effective_function_declarations_rule(css_rules(), nullptr, [&](CSSFunctionDeclarations const& declarations, GC::Ptr<CSSContainerRule const> container_rule) {
        if (container_rule) {
            container_rule->mark_element_style_dependencies(root_element);
            if (!container_rule->matches(root_element))
                return;
        }
        for (auto const& descriptor : declarations.style()->descriptors())
            callback(descriptor.descriptor_name_and_id.name(), descriptor.value);
    });
}

}
