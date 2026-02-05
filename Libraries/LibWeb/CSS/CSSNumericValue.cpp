/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/Bindings/CSSNumericValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSMathInvert.h>
#include <LibWeb/CSS/CSSMathMax.h>
#include <LibWeb/CSS/CSSMathMin.h>
#include <LibWeb/CSS/CSSMathNegate.h>
#include <LibWeb/CSS/CSSMathProduct.h>
#include <LibWeb/CSS/CSSMathSum.h>
#include <LibWeb/CSS/CSSMathValue.h>
#include <LibWeb/CSS/CSSNumericArray.h>
#include <LibWeb/CSS/CSSNumericValue.h>
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

static bool all_values_are_css_unit_values_with_the_same_unit(GC::RootVector<GC::Ref<CSSNumericValue>> const& values)
{
    VERIFY(!values.is_empty());
    return all_of(values, [&](auto& value) {
        if (auto* unit_value = as_if<CSSUnitValue>(*value))
            return unit_value->unit() == as<CSSUnitValue>(*values[0]).unit();
        return false;
    });
}

template<typename Operation>
static GC::Ref<CSSNumericValue> apply_math_operation_on_css_unit_values(JS::Realm& realm, GC::RootVector<GC::Ref<CSSNumericValue>> const& values, Operation&& operation)
{
    auto& first_unit_value = as<CSSUnitValue>(*values[0]);
    auto& unit = first_unit_value.unit();

    double result = first_unit_value.value();
    for (size_t i = 1; i < values.size(); ++i)
        result = operation(result, as<CSSUnitValue>(*values[i]).value());
    return CSSUnitValue::create(realm, result, unit);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-add
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::add(Vector<CSSNumberish> const& initial_values)
{
    auto& realm = this->realm();

    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathSum object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values { heap() };
    if (auto const* math_sum = as_if<CSSMathSum>(*this))
        values.extend(math_sum->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(realm, value));

    // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose unit
    //    internal slot is set to that unit, and value internal slot is set to the sum of the value internal slots of
    //    the items in values. This addition must be done "left to right" - if values is « 1, 2, 3, 4 », the result must
    //    be (((1 + 2) + 3) + 4). (This detail is necessary to ensure interoperability in the presence of floating-point
    //    arithmetic.)
    if (all_values_are_css_unit_values_with_the_same_unit(values))
        return apply_math_operation_on_css_unit_values(realm, values, [](double a, double b) { return a + b; });

    // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
    // 5. Return a new CSSMathSum object whose values internal slot is set to values.
    return TRY(CSSMathSum::add_all_types_into_math_sum(realm, values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-sub
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::sub(Vector<CSSNumberish> const& initial_values)
{
    auto& realm = this->realm();

    // 1. Replace each item of values with the result of rectifying a numberish value for the item, then negating the value.
    Vector<CSSNumberish> values;
    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(realm, value)->negate());

    // 2. Return the result of calling the add() internal algorithm with this and values.
    return add(values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssmath-negate-a-cssnumericvalue
CSSNumberish CSSNumericValue::negate()
{
    // 1. If this is a CSSMathNegate object, return this’s value internal slot.
    if (auto* negate = as_if<CSSMathNegate>(*this))
        return GC::Root<CSSNumericValue> { negate->value().ptr() };

    // 2. If this is a CSSUnitValue object, return a new CSSUnitValue with the same unit internal slot as this, and a
    //    value internal slot set to the negation of this’s.
    if (auto* unit_value = as_if<CSSUnitValue>(*this))
        return GC::Root<CSSNumericValue> { CSSUnitValue::create(realm(), -unit_value->value(), unit_value->unit()).ptr() };

    // 3. Otherwise, return a new CSSMathNegate object whose value internal slot is set to this.
    return GC::Root<CSSNumericValue> { CSSMathNegate::construct_impl(realm(), GC::Root<CSSNumericValue> { this }).ptr() };
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-mul
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::mul(Vector<CSSNumberish> const& initial_values)
{
    auto& realm = this->realm();
    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathProduct object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values { heap() };
    if (auto const* math_product = as_if<CSSMathProduct>(*this))
        values.extend(math_product->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(realm, value));

    // 3. If all of the items in values are CSSUnitValues with unit internal slot set to "number", return a new
    //    CSSUnitValue whose unit internal slot is set to "number", and value internal slot is set to the product of the
    //    value internal slots of the items in values.
    //
    //    This multiplication must be done "left to right" - if values is « 1, 2, 3, 4 », the result must be (((1 × 2) × 3) × 4).
    //    (This detail is necessary to ensure interoperability in the presence of floating-point arithmetic.)
    //
    // 4. If all of the items in values are CSSUnitValues with unit internal slot set to "number" except one which is
    //    set to unit, return a new CSSUnitValue whose unit internal slot is set to unit, and value internal slot is set
    //    to the product of the value internal slots of the items in values.
    //
    //    This multiplication must be done "left to right" - if values is « 1, 2, 3, 4 », the result must be (((1 × 2) × 3) × 4).
    bool all_values_are_units = all_of(values, [](auto& value) {
        return is<CSSUnitValue>(*value);
    });

    if (all_values_are_units) {
        bool multiple_units_found = false;
        Optional<size_t> non_number_unit_index;
        for (size_t i = 0; i < values.size(); ++i) {
            auto unit = as<CSSUnitValue>(*values[i]).unit();
            if (unit == "number"sv)
                continue;
            if (non_number_unit_index.has_value()) {
                multiple_units_found = true;
                break;
            }
            non_number_unit_index = i;
        }
        if (!multiple_units_found) {
            double product = 1;
            for (auto& value : values)
                product *= as<CSSUnitValue>(*value).value();
            auto unit = non_number_unit_index.has_value() ? as<CSSUnitValue>(*values[*non_number_unit_index]).unit() : "number"_fly_string;
            return CSSUnitValue::create(realm, product, unit);
        }
    }

    // 5. Let type be the result of multiplying the types of every item in values. If type is failure, throw a TypeError.
    // 6. Return a new CSSMathProduct object whose values internal slot is set to values.
    return TRY(CSSMathProduct::multiply_all_types_into_math_product(realm, values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-div
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::div(Vector<CSSNumberish> const& initial_values)
{
    auto& realm = this->realm();

    // 1. Replace each item of values with the result of rectifying a numberish value for the item, then inverting the value.
    Vector<CSSNumberish> values;
    for (auto const& value : initial_values)
        values.append(TRY(rectify_a_numberish_value(realm, value)->invert()));

    // 2. Return the result of calling the mul() internal algorithm with this and values.
    return mul(values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssmath-invert-a-cssnumericvalue
WebIDL::ExceptionOr<CSSNumberish> CSSNumericValue::invert()
{
    // 1. If this is a CSSMathInvert object, return this’s value internal slot.
    if (auto* invert = as_if<CSSMathInvert>(*this))
        return GC::Root<CSSNumericValue> { invert->value().ptr() };

    // 2. If this is a CSSUnitValue object with unit internal slot set to "number":
    if (auto* unit_value = as_if<CSSUnitValue>(*this); unit_value && unit_value->unit() == "number"sv) {
        // 1. If this’s value internal slot is set to 0 or -0, throw a RangeError.
        if (unit_value->value() == 0 || unit_value->value() == -0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Zero has no multiplicative inverse"sv };

        // 2. Else return a new CSSUnitValue with the unit internal slot set to "number", and a value internal slot set
        //    to 1 divided by this’s {CSSUnitValue/value}} internal slot.
        return GC::Root<CSSNumericValue> { CSSUnitValue::create(realm(), 1.0 / unit_value->value(), "number"_fly_string).ptr() };
    }

    // 3. Otherwise, return a new CSSMathInvert object whose value internal slot is set to this.
    return GC::Root<CSSNumericValue> { CSSMathInvert::construct_impl(realm(), GC::Root<CSSNumericValue> { this }).ptr() };
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-min
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::min(Vector<CSSNumberish> const& initial_values)
{
    auto& realm = this->realm();

    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathMin object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values { heap() };
    if (auto const* math_product = as_if<CSSMathMin>(*this))
        values.extend(math_product->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(realm, value));

    // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose unit
    //    internal slot is set to that unit, and value internal slot is set to the minimum of the value internal slots
    //    of the items in values.
    if (all_values_are_css_unit_values_with_the_same_unit(values))
        return apply_math_operation_on_css_unit_values(realm, values, [](double a, double b) { return AK::min(a, b); });

    // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
    // 5. Return a new CSSMathMin object whose values internal slot is set to values.
    return TRY(CSSMathMin::add_all_types_into_math_min(realm, values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-max
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::max(Vector<CSSNumberish> const& initial_values)
{
    auto& realm = this->realm();

    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathMax object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values { heap() };
    if (auto const* math_product = as_if<CSSMathMax>(*this))
        values.extend(math_product->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(realm, value));

    // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose unit
    //    internal slot is set to that unit, and value internal slot is set to the maximum of the value internal slots
    //    of the items in values.
    if (all_values_are_css_unit_values_with_the_same_unit(values))
        return apply_math_operation_on_css_unit_values(realm, values, [](double a, double b) { return AK::max(a, b); });

    // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
    // 5. Return a new CSSMathMax object whose values internal slot is set to values.
    return TRY(CSSMathMax::add_all_types_into_math_max(realm, values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-equals
bool CSSNumericValue::equals_for_bindings(Vector<CSSNumberish> values) const
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
void CSSNumericValue::serialize(StringBuilder& builder, SerializationParams const& params) const
{
    // To serialize a CSSNumericValue this, given an optional minimum, a numeric value, and optional maximum, a numeric value:
    // 1. If this is a CSSUnitValue, serialize a CSSUnitValue from this, passing minimum and maximum. Return the result.
    if (auto* unit_value = as_if<CSSUnitValue>(this)) {
        unit_value->serialize_unit_value(builder, params.minimum, params.maximum);
        return;
    }
    // 2. Otherwise, serialize a CSSMathValue from this, and return the result.
    auto& math_value = as<CSSMathValue>(*this);
    math_value.serialize_math_value(builder,
        params.nested ? CSSMathValue::Nested::Yes : CSSMathValue::Nested::No,
        params.parenless ? CSSMathValue::Parens::Without : CSSMathValue::Parens::With);
}

String CSSNumericValue::to_string(SerializationParams const& params) const
{
    StringBuilder builder;
    serialize(builder, params);
    return builder.to_string_without_validation();
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
