/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNumericValue.h"
#include <LibWeb/Bindings/CSSNumericValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSMathValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/MathFunctions.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNumericValue);

static Bindings::CSSNumericBaseType to_om_numeric_base_type(NumericType::BaseType source)
{
    switch (source) {
    case NumericType::BaseType::Length:
        return Bindings::CSSNumericBaseType::Length;
    case NumericType::BaseType::Angle:
        return Bindings::CSSNumericBaseType::Angle;
    case NumericType::BaseType::Time:
        return Bindings::CSSNumericBaseType::Time;
    case NumericType::BaseType::Frequency:
        return Bindings::CSSNumericBaseType::Frequency;
    case NumericType::BaseType::Resolution:
        return Bindings::CSSNumericBaseType::Resolution;
    case NumericType::BaseType::Flex:
        return Bindings::CSSNumericBaseType::Flex;
    case NumericType::BaseType::Percent:
        return Bindings::CSSNumericBaseType::Percent;
    case NumericType::BaseType::__Count:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

CSSNumericValue::CSSNumericValue(JS::Realm& realm, NumericType type)
    : CSSStyleValue(realm)
    , m_type(move(type))
{
}

void CSSNumericValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSNumericValue);
    Base::initialize(realm);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-equals
bool CSSNumericValue::equals_for_bindings(Vector<CSSNumberish> const& values) const
{
    // The equals(...values) method, when called on a CSSNumericValue this, must perform the following steps:

    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. For each item in values, if the item is not an equal numeric value to this, return false.
    for (auto const& value : values) {
        auto rectified_value = rectify_a_numberish_value(realm(), value);
        if (!is_equal_numeric_value(rectified_value))
            return false;
    }

    // 3. Return true.
    return true;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-to
WebIDL::ExceptionOr<GC::Ref<CSSUnitValue>> CSSNumericValue::to(FlyString const& unit) const
{
    // The to(unit) method converts an existing CSSNumericValue this into another one with the specified unit, if
    // possible. When called, it must perform the following steps:

    // 1. Let type be the result of creating a type from unit. If type is failure, throw a SyntaxError.
    auto maybe_type = NumericType::create_from_unit(unit);
    if (!maybe_type.has_value())
        return WebIDL::SyntaxError::create(realm(), Utf16String::formatted("Unrecognized unit '{}'", unit));

    // 2. Let sum be the result of creating a sum value from this. If sum is failure, throw a TypeError.
    auto sum = create_a_sum_value();
    if (!sum.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to create a sum from input '{}'", MUST(to_string()))) };

    // 3. If sum has more than one item, throw a TypeError.
    //    Otherwise, let item be the result of creating a CSSUnitValue from the sole item in sum, then converting it to
    //    unit. If item is failure, throw a TypeError.
    if (sum->size() > 1)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Sum contains more than one item"sv };
    auto item = CSSUnitValue::create_from_sum_value_item(realm(), sum->first());
    if (!item)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to create CSSUnitValue from input '{}'", MUST(to_string()))) };

    auto converted_item = item->converted_to_unit(unit);
    if (!converted_item)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Unable to convert input '{}' to unit '{}'", MUST(to_string()), unit)) };

    // 4. Return item.
    return converted_item.as_nonnull();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-type
CSSNumericType CSSNumericValue::type_for_bindings() const
{
    // 1. Let result be a new CSSNumericType.
    CSSNumericType result {};

    // 2. For each baseType → power in the type of this,
    m_type.for_each_type_and_exponent([&result](NumericType::BaseType base_type, auto power) {
        // 1. If power is not 0, set result[baseType] to power.
        if (power == 0)
            return;

        switch (base_type) {
        case NumericType::BaseType::Length:
            result.length = power;
            break;
        case NumericType::BaseType::Angle:
            result.angle = power;
            break;
        case NumericType::BaseType::Time:
            result.time = power;
            break;
        case NumericType::BaseType::Frequency:
            result.frequency = power;
            break;
        case NumericType::BaseType::Resolution:
            result.resolution = power;
            break;
        case NumericType::BaseType::Flex:
            result.flex = power;
            break;
        case NumericType::BaseType::Percent:
            result.percent = power;
            break;
        case NumericType::BaseType::__Count:
            VERIFY_NOT_REACHED();
        }
    });

    // 3. If the percent hint of this is not null,
    if (auto percent_hint = m_type.percent_hint(); percent_hint.has_value()) {
        // 1. Set result[percentHint] to the percent hint of this.
        result.percent_hint = to_om_numeric_base_type(percent_hint.value());
    }

    // 4. Return result.
    return result;
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssnumericvalue
String CSSNumericValue::to_string(SerializationParams const& params) const
{
    // To serialize a CSSNumericValue this, given an optional minimum, a numeric value, and optional maximum, a numeric value:
    // 1. If this is a CSSUnitValue, serialize a CSSUnitValue from this, passing minimum and maximum. Return the result.
    if (auto* unit_value = as_if<CSSUnitValue>(this)) {
        return unit_value->serialize_unit_value(params.minimum, params.maximum);
    }
    // 2. Otherwise, serialize a CSSMathValue from this, and return the result.
    auto& math_value = as<CSSMathValue>(*this);
    return math_value.serialize_math_value(
        params.nested ? CSSMathValue::Nested::Yes : CSSMathValue::Nested::No,
        params.parenless ? CSSMathValue::Parens::Without : CSSMathValue::Parens::With);
}

// https://drafts.css-houdini.org/css-typed-om-1/#rectify-a-numberish-value
GC::Ref<CSSNumericValue> rectify_a_numberish_value(JS::Realm& realm, CSSNumberish const& numberish, Optional<FlyString> unit)
{
    // To rectify a numberish value num, optionally to a given unit unit (defaulting to "number"), perform the following steps:
    return numberish.visit(
        // 1. If num is a CSSNumericValue, return num.
        [](GC::Root<CSSNumericValue> const& num) -> GC::Ref<CSSNumericValue> {
            return GC::Ref { *num };
        },
        // 2. If num is a double, return a new CSSUnitValue with its value internal slot set to num and its unit
        //    internal slot set to unit.
        [&realm, &unit](double num) -> GC::Ref<CSSNumericValue> {
            return CSSUnitValue::create(realm, num, unit.value_or("number"_fly_string));
        });
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-numeric-value
static WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> reify_a_numeric_value(JS::Realm& realm, Parser::ComponentValue const& numeric_value)
{
    // To reify a numeric value num:
    // 1. If num is a math function, reify a math expression from num and return the result.
    if (numeric_value.is_function()) {
        // AD-HOC: The only feasible way is to parse it as a StyleValue and rely on the reification code there.
        auto parser = Parser::Parser::create(Parser::ParsingParams {}, {});
        if (auto calculation = parser.parse_calculated_value(numeric_value)) {
            auto reified = calculation->reify(realm, {});
            // AD-HOC: Not all math functions can be reified. Until we have clear guidance on that, throw a SyntaxError.
            // See: https://github.com/w3c/css-houdini-drafts/issues/1090#issuecomment-3200229996
            if (auto* reified_numeric = as_if<CSSNumericValue>(*reified)) {
                return GC::Ref { *reified_numeric };
            }
            return WebIDL::SyntaxError::create(realm, "Unable to reify this math function."_utf16);
        }
        // AD-HOC: If we failed to parse it, I guess we throw a SyntaxError like in step 1 of CSSNumericValue::parse().
        return WebIDL::SyntaxError::create(realm, "Unable to parse input as a calculation tree."_utf16);
    }

    // 2. If num is the unitless value 0 and num is a <dimension>, return a new CSSUnitValue with its value internal
    //    slot set to 0, and its unit internal slot set to "px".
    // FIXME: What does this mean? We just have a component value, it doesn't have any knowledge about whether 0 should
    //        be interpreted as a dimension.

    // 3. Return a new CSSUnitValue with its value internal slot set to the numeric value of num, and its unit internal
    //    slot set to "number" if num is a <number>, "percent" if num is a <percentage>, and num’s unit if num is a
    //    <dimension>.
    //    If the value being reified is a computed value, the unit used must be the appropriate canonical unit for the
    //    value’s type, with the numeric value scaled accordingly.
    // NB: The computed value part is irrelevant here, I think.
    if (numeric_value.is(Parser::Token::Type::Number))
        return CSSUnitValue::create(realm, numeric_value.token().number_value(), "number"_fly_string);
    if (numeric_value.is(Parser::Token::Type::Percentage))
        return CSSUnitValue::create(realm, numeric_value.token().percentage(), "percent"_fly_string);
    VERIFY(numeric_value.is(Parser::Token::Type::Dimension));
    return CSSUnitValue::create(realm, numeric_value.token().dimension_value(), numeric_value.token().dimension_unit());
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-parse
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::parse(JS::VM& vm, String const& css_text)
{
    // The parse(cssText) method, when called, must perform the following steps:

    auto& realm = *vm.current_realm();

    // 1. Parse a component value from cssText and let result be the result. If result is a syntax error, throw a
    //    SyntaxError and abort this algorithm.
    auto maybe_component_value = Parser::Parser::create(Parser::ParsingParams {}, css_text).parse_as_component_value();
    if (!maybe_component_value.has_value()) {
        return WebIDL::SyntaxError::create(realm, "Unable to parse input as a component value."_utf16);
    }
    auto& result = maybe_component_value.value();

    // 2. If result is not a <number-token>, <percentage-token>, <dimension-token>, or a math function, throw a
    //    SyntaxError and abort this algorithm.
    auto is_a_math_function = [](Parser::ComponentValue const& component_value) -> bool {
        if (!component_value.is_function())
            return false;
        return math_function_from_string(component_value.function().name).has_value();
    };
    if (!(result.is(Parser::Token::Type::Number)
            || result.is(Parser::Token::Type::Percentage)
            || result.is(Parser::Token::Type::Dimension)
            || is_a_math_function(result))) {
        return WebIDL::SyntaxError::create(realm, "Input not a <number-token>, <percentage-token>, <dimension-token>, or a math function."_utf16);
    }

    // 3. If result is a <dimension-token> and creating a type from result’s unit returns failure, throw a SyntaxError
    //    and abort this algorithm.
    if (result.is(Parser::Token::Type::Dimension)) {
        if (!NumericType::create_from_unit(result.token().dimension_unit()).has_value()) {
            return WebIDL::SyntaxError::create(realm, "Input is <dimension> with an unrecognized unit."_utf16);
        }
    }

    // 4. Reify a numeric value result, and return the result.
    return reify_a_numeric_value(realm, result);
}

}
