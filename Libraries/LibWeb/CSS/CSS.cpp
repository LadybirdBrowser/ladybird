/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSS.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#dom-css-escape
WebIDL::ExceptionOr<String> escape(JS::VM&, StringView identifier)
{
    // The escape(ident) operation must return the result of invoking serialize an identifier of ident.
    return serialize_an_identifier(identifier);
}

// https://www.w3.org/TR/css-conditional-3/#dom-css-supports
bool supports(JS::VM&, FlyString const& property_name, StringView value)
{
    // 1. If property is an ASCII case-insensitive match for any defined CSS property that the UA supports, or is a
    //    custom property name string, and value successfully parses according to that property’s grammar, return true.
    if (auto property = PropertyNameAndID::from_name(property_name); property.has_value()) {
        if (parse_css_value(Parser::ParsingParams {}, value, property->id()))
            return true;
    }

    // 2. Otherwise, return false.
    return false;
}

// https://www.w3.org/TR/css-conditional-3/#dom-css-supports
WebIDL::ExceptionOr<bool> supports(JS::VM& vm, StringView condition_text)
{
    auto& realm = *vm.current_realm();

    // 1. If conditionText, parsed and evaluated as a <supports-condition>, would return true, return true.
    if (auto supports = parse_css_supports(Parser::ParsingParams { realm }, condition_text); supports && supports->matches())
        return true;

    // 2. Otherwise, If conditionText, wrapped in parentheses and then parsed and evaluated as a <supports-condition>, would return true, return true.
    auto wrapped_condition_text = TRY_OR_THROW_OOM(vm, String::formatted("({})", condition_text));

    if (auto supports = parse_css_supports(Parser::ParsingParams { realm }, wrapped_condition_text); supports && supports->matches())
        return true;

    // 3. Otherwise, return false.
    return false;
}

// https://www.w3.org/TR/css-properties-values-api-1/#the-registerproperty-function
WebIDL::ExceptionOr<void> register_property(JS::VM& vm, PropertyDefinition definition)
{
    // 1. Let property set be the value of the current global object’s associated Document’s [[registeredPropertySet]] slot.
    auto& realm = *vm.current_realm();
    auto& window = static_cast<Web::HTML::Window&>(realm.global_object());
    auto& document = window.associated_document();
    auto& property_set = document.registered_property_set();

    // 2. If name is not a custom property name string, throw a SyntaxError and exit this algorithm.
    if (!is_a_custom_property_name_string(definition.name))
        return WebIDL::SyntaxError::create(realm, "Invalid property name"_utf16);

    // If property set already contains an entry with name as its property name (compared codepoint-wise),
    // throw an InvalidModificationError and exit this algorithm.
    if (property_set.contains(definition.name))
        return WebIDL::InvalidModificationError::create(realm, "Property already registered"_utf16);

    auto parsing_params = CSS::Parser::ParsingParams { document };

    // 3. Attempt to consume a syntax definition from syntax. If it returns failure, throw a SyntaxError.
    //    Otherwise, let syntax definition be the returned syntax definition.
    auto syntax_component_values = parse_component_values_list(parsing_params, definition.syntax);
    auto maybe_syntax = parse_as_syntax(syntax_component_values);
    if (!maybe_syntax) {
        return WebIDL::SyntaxError::create(realm, "Invalid syntax definition"_utf16);
    }

    RefPtr<StyleValue const> initial_value_maybe;

    // 4. If syntax definition is the universal syntax definition, and initialValue is not present,
    if (maybe_syntax->type() == Parser::SyntaxNode::NodeType::Universal) {
        if (!definition.initial_value.has_value()) {
            // let parsed initial value be empty.
            // This must be treated identically to the "default" initial value of custom properties, as defined in [css-variables].
            initial_value_maybe = nullptr;
        } else {
            // Otherwise, if syntax definition is the universal syntax definition,
            // parse initialValue as a <declaration-value>
            initial_value_maybe = parse_css_value(parsing_params, definition.initial_value.value(), PropertyID::Custom);
            // If this fails, throw a SyntaxError and exit this algorithm.
            // Otherwise, let parsed initial value be the parsed result.
            if (!initial_value_maybe) {
                return WebIDL::SyntaxError::create(realm, "Invalid initial value"_utf16);
            }
        }
    } else if (!definition.initial_value.has_value()) {
        // Otherwise, if initialValue is not present, throw a SyntaxError and exit this algorithm.
        return WebIDL::SyntaxError::create(realm, "Initial value must be provided for non-universal syntax"_utf16);
    } else {
        // Otherwise, parse initialValue according to syntax definition.
        auto initial_value_component_values = parse_component_values_list(parsing_params, definition.initial_value.value());

        initial_value_maybe = Parser::parse_with_a_syntax(
            parsing_params,
            initial_value_component_values,
            *maybe_syntax);

        // If this fails, throw a SyntaxError and exit this algorithm.
        if (!initial_value_maybe || initial_value_maybe->is_guaranteed_invalid()) {
            return WebIDL::SyntaxError::create(realm, "Invalid initial value"_utf16);
        }
        // Otherwise, let parsed initial value be the parsed result.
        // NB: Already done

        // FIXME: If parsed initial value is not computationally independent, throw a SyntaxError and exit this algorithm.
    }

    // 5. Set inherit flag to the value of inherits.
    // NB: Combined with 6.

    // 6. Let registered property be a struct with a property name of name, a syntax of syntax definition,
    //    an initial value of parsed initial value, and an inherit flag of inherit flag.
    CustomPropertyRegistration registered_property {
        .property_name = definition.name,
        .syntax = definition.syntax,
        .inherit = definition.inherits,
        .initial_value = initial_value_maybe,
    };
    // Append registered property to property set.
    property_set.set(registered_property.property_name, registered_property);

    return {};
}

}
