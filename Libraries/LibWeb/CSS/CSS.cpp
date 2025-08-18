/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSS.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#dom-css-escape
WebIDL::ExceptionOr<String> escape(JS::VM&, StringView identifier)
{
    // The escape(ident) operation must return the result of invoking serialize an identifier of ident.
    return serialize_an_identifier(identifier);
}

// https://www.w3.org/TR/css-conditional-3/#dom-css-supports
bool supports(JS::VM&, StringView property, StringView value)
{
    // 1. If property is an ASCII case-insensitive match for any defined CSS property that the UA supports,
    //    and value successfully parses according to that property’s grammar, return true.
    if (auto property_id = property_id_from_string(property); property_id.has_value()) {
        if (parse_css_value(Parser::ParsingParams {}, value, property_id.value()))
            return true;
    }

    // 2. Otherwise, if property is a custom property name string, return true.
    else if (is_a_custom_property_name_string(property)) {
        return true;
    }

    // 3. Otherwise, return false.
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

    // 2. If name is not a custom property name string, throw a SyntaxError and exit this algorithm.
    if (!is_a_custom_property_name_string(definition.name))
        return WebIDL::SyntaxError::create(realm, "Invalid property name"_utf16);

    // If property set already contains an entry with name as its property name (compared codepoint-wise),
    // throw an InvalidModificationError and exit this algorithm.
    if (document.registered_custom_properties().contains(definition.name))
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
    auto registered_property = CSSPropertyRule::create(realm, definition.name, definition.syntax, definition.inherits, initial_value_maybe);
    // Append registered property to property set.
    document.registered_custom_properties().set(
        registered_property->name(),
        registered_property);

    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#numeric-factory
inline GC::Ref<CSSUnitValue> numeric_factory(JS::VM& vm, WebIDL::Double value, FlyString unit)
{
    // All of the above methods must, when called with a double value, return a new CSSUnitValue whose value internal
    // slot is set to value and whose unit internal slot is set to the name of the method as defined here.
    return CSSUnitValue::create(*vm.current_realm(), value, move(unit));
}

GC::Ref<CSSUnitValue> number(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "number"_fly_string);
}

GC::Ref<CSSUnitValue> percent(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "percent"_fly_string);
}

// <length>
GC::Ref<CSSUnitValue> cap(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cap"_fly_string);
}

GC::Ref<CSSUnitValue> ch(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "ch"_fly_string);
}

GC::Ref<CSSUnitValue> em(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "em"_fly_string);
}

GC::Ref<CSSUnitValue> ex(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "ex"_fly_string);
}

GC::Ref<CSSUnitValue> ic(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "ic"_fly_string);
}

GC::Ref<CSSUnitValue> lh(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "lh"_fly_string);
}

GC::Ref<CSSUnitValue> rcap(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "rcap"_fly_string);
}

GC::Ref<CSSUnitValue> rch(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "rch"_fly_string);
}

GC::Ref<CSSUnitValue> rem(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "rem"_fly_string);
}

GC::Ref<CSSUnitValue> rex(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "rex"_fly_string);
}

GC::Ref<CSSUnitValue> ric(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "ric"_fly_string);
}

GC::Ref<CSSUnitValue> rlh(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "rlh"_fly_string);
}

GC::Ref<CSSUnitValue> vw(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "vw"_fly_string);
}

GC::Ref<CSSUnitValue> vh(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "vh"_fly_string);
}

GC::Ref<CSSUnitValue> vi(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "vi"_fly_string);
}

GC::Ref<CSSUnitValue> vb(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "vb"_fly_string);
}

GC::Ref<CSSUnitValue> vmin(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "vmin"_fly_string);
}

GC::Ref<CSSUnitValue> vmax(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "vmax"_fly_string);
}

GC::Ref<CSSUnitValue> svw(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "svw"_fly_string);
}

GC::Ref<CSSUnitValue> svh(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "svh"_fly_string);
}

GC::Ref<CSSUnitValue> svi(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "svi"_fly_string);
}

GC::Ref<CSSUnitValue> svb(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "svb"_fly_string);
}

GC::Ref<CSSUnitValue> svmin(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "svmin"_fly_string);
}

GC::Ref<CSSUnitValue> svmax(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "svmax"_fly_string);
}

GC::Ref<CSSUnitValue> lvw(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "lvw"_fly_string);
}

GC::Ref<CSSUnitValue> lvh(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "lvh"_fly_string);
}

GC::Ref<CSSUnitValue> lvi(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "lvi"_fly_string);
}

GC::Ref<CSSUnitValue> lvb(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "lvb"_fly_string);
}

GC::Ref<CSSUnitValue> lvmin(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "lvmin"_fly_string);
}

GC::Ref<CSSUnitValue> lvmax(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "lvmax"_fly_string);
}

GC::Ref<CSSUnitValue> dvw(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dvw"_fly_string);
}

GC::Ref<CSSUnitValue> dvh(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dvh"_fly_string);
}

GC::Ref<CSSUnitValue> dvi(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dvi"_fly_string);
}

GC::Ref<CSSUnitValue> dvb(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dvb"_fly_string);
}

GC::Ref<CSSUnitValue> dvmin(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dvmin"_fly_string);
}

GC::Ref<CSSUnitValue> dvmax(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dvmax"_fly_string);
}

GC::Ref<CSSUnitValue> cqw(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cqw"_fly_string);
}

GC::Ref<CSSUnitValue> cqh(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cqh"_fly_string);
}

GC::Ref<CSSUnitValue> cqi(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cqi"_fly_string);
}

GC::Ref<CSSUnitValue> cqb(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cqb"_fly_string);
}

GC::Ref<CSSUnitValue> cqmin(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cqmin"_fly_string);
}

GC::Ref<CSSUnitValue> cqmax(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cqmax"_fly_string);
}

GC::Ref<CSSUnitValue> cm(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "cm"_fly_string);
}

GC::Ref<CSSUnitValue> mm(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "mm"_fly_string);
}

GC::Ref<CSSUnitValue> q(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "q"_fly_string);
}

GC::Ref<CSSUnitValue> in(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "in"_fly_string);
}

GC::Ref<CSSUnitValue> pt(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "pt"_fly_string);
}

GC::Ref<CSSUnitValue> pc(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "pc"_fly_string);
}

GC::Ref<CSSUnitValue> px(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "px"_fly_string);
}

// <angle>
GC::Ref<CSSUnitValue> deg(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "deg"_fly_string);
}

GC::Ref<CSSUnitValue> grad(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "grad"_fly_string);
}

GC::Ref<CSSUnitValue> rad(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "rad"_fly_string);
}

GC::Ref<CSSUnitValue> turn(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "turn"_fly_string);
}

// <time>
GC::Ref<CSSUnitValue> s(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "s"_fly_string);
}

GC::Ref<CSSUnitValue> ms(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "ms"_fly_string);
}

// <frequency>
GC::Ref<CSSUnitValue> hz(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "hz"_fly_string);
}

GC::Ref<CSSUnitValue> k_hz(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "khz"_fly_string);
}

// <resolution>
GC::Ref<CSSUnitValue> dpi(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dpi"_fly_string);
}

GC::Ref<CSSUnitValue> dpcm(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dpcm"_fly_string);
}

GC::Ref<CSSUnitValue> dppx(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "dppx"_fly_string);
}

// <flex>
GC::Ref<CSSUnitValue> fr(JS::VM& vm, WebIDL::Double value)
{
    return numeric_factory(vm, value, "fr"_fly_string);
}

}
