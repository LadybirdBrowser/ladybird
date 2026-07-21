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
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
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
        internal.type->to_string(),

        // defaultValue
        // The default value of the function parameter, or `null` if the argument does not have a default.
        internal.default_value
            ? internal.default_value->to_utf16_string(SerializationMode::Normal)
            : Optional<Utf16String> {},
    };
}

// https://drafts.csswg.org/css-mixins-1/#serialize-a-css-type
static void serialize_a_css_type(Utf16StringBuilder& builder, Parser::SyntaxNode const& type)
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
    builder.append_ascii("type("sv);

    // The corresponding syntax string.
    builder.append(type.to_string());

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
    if (type->type() != Parser::SyntaxNode::NodeType::Universal) {
        // - A single SPACE (U+0020), followed by the result of performing serialize a CSS type on that type.
        builder.append_ascii(' ');
        serialize_a_css_type(builder, *type);
    }

    // If the function parameter has a default value:
    if (default_value) {
        // - A single COLON (U+003A), followed by a single SPACE (U+0020), followed by the result of performing
        //   serialize a CSS value on that value.
        builder.appendff(": {}", default_value->to_string(SerializationMode::Normal));
    }
}

GC::Ref<CSSFunctionRule> CSSFunctionRule::create(JS::Realm& realm, CSSRuleList& rules, Utf16FlyString name, Vector<FunctionParameterInternal> parameters, NonnullRefPtr<Parser::SyntaxNode> return_type)
{
    return realm.create<CSSFunctionRule>(realm, rules, move(name), move(parameters), move(return_type));
}

CSSFunctionRule::CSSFunctionRule(JS::Realm& realm, CSSRuleList& rules, Utf16FlyString name, Vector<FunctionParameterInternal> parameters, NonnullRefPtr<Parser::SyntaxNode> return_type)
    : CSSGroupingRule(realm, rules, Type::Function)
    , m_name(move(name))
    , m_parameters(move(parameters))
    , m_return_type(move(return_type))
{
}

Utf16String CSSFunctionRule::name() const
{
    return m_name.to_utf16_string();
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
Utf16String CSSFunctionRule::return_type() const
{
    // The return type of the custom function, represented as a syntax string. If the custom function has no return
    // type, returns "*".
    // NB: We always store a return type (defaulting to "*")
    return m_return_type->to_string();
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
    if (m_return_type->type() != Parser::SyntaxNode::NodeType::Universal) {
        // - A single SPACE (U+0020), followed by the string "returns", followed by a single SPACE (U+0020).
        builder.append_ascii(" returns "sv);

        // - The result of performing serialize a CSS type on that type.
        serialize_a_css_type(builder, *m_return_type);
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

// https://drafts.csswg.org/css-mixins/#evaluate-a-custom-function
NonnullRefPtr<StyleValue const> CSSFunctionRule::evaluate_a_custom_function(Parser::GuardedSubstitutionContexts& guarded_contexts, Vector<Vector<Parser::ComponentValue>> const& arguments, CallingContext const& calling_context) const
{
    // 1. Let substitution context be a substitution context containing «"function", custom function».
    // Note: Due to tree-scoping, the same function name may appear multiple times on the stack while referring to
    //       different custom functions. For this reason, the custom function itself is included in the substitution
    //       context, not just its name.
    Parser::SubstitutionContext substitution_context { Parser::FunctionSubstitutionContextDependency { .custom_function = *this } };

    // 2. Guard substitution context for the remainder of this algorithm. If substitution context is marked as cyclic,
    //    return the guaranteed-invalid value.
    guarded_contexts.guard(substitution_context);

    if (substitution_context.is_cyclic)
        return GuaranteedInvalidStyleValue::create();

    ScopeGuard const guard { [&] {
        guarded_contexts.unguard(substitution_context);
    } };

    // 3. If the number of items in arguments is greater than the number of function parameters in custom function,
    //    return the guaranteed-invalid value.
    if (arguments.size() > m_parameters.size())
        return GuaranteedInvalidStyleValue::create();

    // 4. Let registrations be an initially empty set of custom property registrations.
    HashMap<Utf16FlyString, CustomPropertyRegistration> registrations;

    // 5. For each function parameter of custom function, create a custom property registration with the parameter’s
    //    name, a syntax of the parameter type, an inherit flag of "true", and no initial value. Add the registration
    //    to registrations.
    for (auto const& parameter : m_parameters) {
        registrations.set(
            parameter.name,
            CustomPropertyRegistration {
                .property_name = parameter.name,
                .syntax = parameter.type,
                .inherit = true,
                .initial_value = nullptr,
            });
    }

    // 6. If custom function has a return type, create a custom property registration with the name "result" (violating
    //    the usual rules for what a registration’s name can be), a syntax of the return type, an inherit flag of
    //    "false", and no initial value. Add the registration to registrations.
    // NB: We always have a return type (defaulting to "*").
    registrations.set(
        "result"_utf16_fly_string,
        CustomPropertyRegistration {
            .property_name = "result"_utf16_fly_string,
            .syntax = m_return_type,
            .inherit = false,
            .initial_value = nullptr,
        });

    // 7. Let argument rule be an initially empty style rule.
    OrderedHashMap<Utf16FlyString, StyleProperty> argument_rule;

    // 8. For each function parameter of custom function:
    for (size_t i = 0; i < m_parameters.size(); ++i) {
        auto const& function_parameter = m_parameters[i];

        // AD-HOC: Chrome (the only other implementer at time of writing) resolves the entire function to the GIV if a
        //         parameter without a default value is omitted. See https://github.com/w3c/csswg-drafts/issues/14165
        if (arguments.size() <= i && !function_parameter.default_value)
            return GuaranteedInvalidStyleValue::create();

        // 1. Let arg value be the value of the corresponding argument in arguments, or the guaranteed-invalid value if
        //    there is no corresponding argument.
        auto const& arg_value = arguments.size() > i
            ? arguments[i]
            : Vector { Parser::ComponentValue { Parser::GuaranteedInvalidValue {} } };

        // 2. Let default value be the parameter’s default value.
        auto const& default_value = function_parameter.default_value;

        // 3. Add a custom property to argument rule with a name of the parameter’s name, and a value of
        //    'first-valid(arg value, default value)'.

        // FIXME: We haven't implemented 'first-valid' yet so we just do the equivalent inline.
        auto parsed_arg_value = [&]() -> NonnullRefPtr<StyleValue const> {
            if (Parser::contains_guaranteed_invalid_value(arg_value))
                return GuaranteedInvalidStyleValue::create();

            return Parser::parse_with_a_syntax(Parser::ParsingParams { calling_context.element.document() }, arg_value, *function_parameter.type);
        }();

        RefPtr<StyleValue const> resolved_first_valid_value;
        if (!parsed_arg_value->is_guaranteed_invalid()) {
            resolved_first_valid_value = parsed_arg_value;
        } else if (default_value) {
            resolved_first_valid_value = *default_value;
        } else {
            resolved_first_valid_value = GuaranteedInvalidStyleValue::create();
        }

        argument_rule.set(function_parameter.name, { .important = Important::No, .property_id = PropertyID::Custom, .value = resolved_first_valid_value.release_nonnull() });
    }

    // 9. Resolve function styles using custom function, argument rule, registrations, and calling context. Let argument
    //    styles be the result.
    auto argument_styles = resolve_function_styles(move(argument_rule), registrations, calling_context, guarded_contexts);

    // 10. Let body rule be the function body of custom function, as a style rule.
    OrderedHashMap<Utf16FlyString, StyleProperty> body_rule;

    auto& root_element = calling_context.element.abstract_element();
    for_each_effective_function_declarations_rule(css_rules(), nullptr, [&](CSSFunctionDeclarations const& declarations, GC::Ptr<CSSContainerRule const> container_rule) {
        if (container_rule) {
            container_rule->mark_element_style_dependencies(root_element);

            if (!container_rule->matches(root_element))
                return;
        }

        for (auto const& descriptor : declarations.style()->descriptors())
            body_rule.set(descriptor.descriptor_name_and_id.name(), { .important = Important::No, .property_id = PropertyID::Custom, .value = descriptor.value });
    });

    // 11. For each custom property registration of registrations except the registration with the name "result", set
    //     its initial value to the corresponding value in argument styles, set its syntax to the universal syntax
    //     definition, and prepend a custom property to body rule with the property name and value in argument styles.
    for (auto& [key, registration] : registrations) {
        if (key == "result"_utf16_fly_string)
            continue;

        auto const& argument_value = argument_styles.get(key).value();

        registration.initial_value = argument_value;

        // AD-HOC: WPT expects us to maintain the syntax - see https://github.com/w3c/csswg-drafts/issues/12315

        if (!body_rule.contains(key))
            body_rule.set(key, { .important = Important::No, .property_id = PropertyID::Custom, .value = *argument_value });
    }

    // 12. Resolve function styles using custom function, body rule, registrations, and calling context. Let body styles
    //     be the result.
    auto body_styles = resolve_function_styles(move(body_rule), registrations, calling_context, guarded_contexts);

    // 13. If substitution context is marked as a cyclic substitution context, return the guaranteed-invalid value.
    // Note: Nested arbitrary substitution functions may have marked substitution context as cyclic at some point after
    //       step 2, for example when resolving result.

    if (substitution_context.is_cyclic)
        return GuaranteedInvalidStyleValue::create();

    // 14. Return the value of the result property in body styles.
    if (auto result = body_styles.get("result"_utf16_fly_string); result.has_value())
        return *result.value();

    // FIXME: We currently only compute declared values - we should instead be computing registered custom properties
    //        regardless of whether they are declared - for now just manually return the GIV if the result wasn't
    //        computed.
    return GuaranteedInvalidStyleValue::create();
}

// https://drafts.csswg.org/css-mixins/#resolve-function-styles
HashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> CSSFunctionRule::resolve_function_styles(OrderedHashMap<Utf16FlyString, StyleProperty>&& custom_properties, HashMap<Utf16FlyString, CustomPropertyRegistration> const& registrations, CallingContext const& calling_context, Parser::GuardedSubstitutionContexts& guarded_contexts) const
{
    auto const& document = calling_context.element.document();

    auto inheritable_custom_property_data = calling_context.element.inheritable_custom_property_data();

    // 1. Create a "hypothetical element" el that acts as a child of calling context’s element. el is featureless, and
    //    only custom properties and the result descriptor apply to it.
    HypotheticalElement hypothetical_element {
        .custom_property_registry = registrations,
        .style_scope = calling_context.style_scope,
        .custom_function = *this,

        // https://drafts.csswg.org/css-mixins/#calling-context-root-element
        // As calling contexts are nested by <dashed-function> evaluations inside of custom functions, a calling
        // context’s root element is the real element at the root of the calling context stack.
        .root_element = calling_context.element.visit(
            [&](DOM::AbstractElement const& abstract_element) { return abstract_element; },
            [&](HypotheticalElement const* hypothetical_element) { return hypothetical_element->root_element; }),
        .parent = calling_context.element,
        .custom_property_data = CustomPropertyData::create({}, inheritable_custom_property_data),
    };

    // 2. Apply rule to el to the specified value stage, with the following changes:

    // - Only the custom property registrations in registrations are visible; all other custom properties are
    //   treated as unregistered.

    //   NB: This is handled by the fact that all registrations are accessed via
    //       AbstractOrHypotheticalElement::custom_property_registration().

    //  - The inherited value of calling context’s property is the guaranteed-invalid value.

    //    NB: This is handled as part of normal context guarding.

    // - On custom properties, the CSS-wide keywords have the following effects:

    //   initial
    //     Resolves to the initial value of the custom property within registrations.

    //   NB: This is handled by the fact that all registrations are accessed via
    //       AbstractOrHypotheticalElement::custom_property_registration().

    //   inherit
    //     Resolves like an inherit() function with the custom property name as its one and only argument.
    //     Note: This ensures that a function parameter defaulted to inherit is reinterpreted using the local
    //           parameter type.

    //   NB: This is handled in inherited_custom_property_value().

    //   any other CSS-wide keyword
    //     Resolves to the guaranteed-invalid value.

    //   NB: This is handled in resolve_css_wide_keyword_for_custom_property()

    //   Note: initial references the custom property registration created from the function parameters, letting
    //         you "reset" a property to the passed value. inherit inherits from the calling context’s element.

    // On result, all CSS-wide keywords are left unresolved.

    // Note: result: inherit, for example, will cause the <dashed-function> to evaluate to the inherit keyword,
    //       similar to var(--unknown, inherit).

    // NB: This is handled in resolve_css_wide_keyword_for_custom_property()

    // - For a given custom property prop, during property replacement for that property, the substitution context
    //   also includes custom function. In other words, the substitution context is «"property", prop’s name, custom
    //   function»

    // Note: Due to dynamic scoping, the same property name may appear multiple times on the stack while referring
    //       to different custom properties. For this reason, the custom function itself is included in the
    //       substitution context, not just its name.

    // NB: This is handled in PropertySubstitutionContextDependency::create()

    hypothetical_element.custom_property_data = CustomPropertyData::create(move(custom_properties), inheritable_custom_property_data, CustomPropertyData::AllowParentOwnValueAbsorption::No);

    // 3. Determine the computed value of all custom properties and the result "property" on el, as defined in CSS
    //    Properties and Values API 1 § 2.4 Computed Value-Time Behavior, with changes from the previous step, and the
    //    following:

    //    - Aside from references to custom properties (which use the values on el as normal) and numbers/percentages
    //      (which are left unresolved in custom properties, as normal), all values which would normally refer to the
    //      element being styled instead refer to calling context’s root element.
    //      Note: For example, attr() in a property, or @container queries in the rule.

    HashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> computed_values;

    for (auto const& [name, property] : hypothetical_element.custom_property_data->own_values()) {
        // FIXME: Can we store the resolved value in CustomPropertyData immediately to avoid recomputing it for any
        //        subsequent properties that depend on it?
        auto computed_value = document.style_computer().compute_value_of_custom_property(calling_context.computed_style_for_custom_property_resolution, AbstractOrHypotheticalElement { hypothetical_element }, name, guarded_contexts);

        computed_values.set(name, computed_value);
    }

    // 4. Return el’s styles.
    //    Note: Only custom properties and the result descriptor will be used from these styles.
    return computed_values;
}

}
