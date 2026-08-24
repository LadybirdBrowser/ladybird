/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CSSNumericValue.h>
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
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNumericValue);

CSSNumericValue::CSSNumericValue(NumericType type)
    : CSSStyleValue()
    , m_type(move(type))
{
}

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

Bindings::CSSNumericType CSSNumericValue::type_for_bindings() const
{
    Bindings::CSSNumericType result {};
    m_type.for_each_type_and_exponent([&result](NumericType::BaseType base_type, auto power) {
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
    if (auto percent_hint = m_type.percent_hint(); percent_hint.has_value())
        result.percent_hint = to_om_numeric_base_type(percent_hint.value());
    return result;
}

static bool all_values_are_css_unit_values_with_the_same_unit(ReadonlySpan<GC::Ref<CSSNumericValue>> const& values)
{
    VERIFY(!values.is_empty());
    return all_of(values, [&](auto& value) {
        if (auto* unit_value = as_if<CSSUnitValue>(*value))
            return unit_value->unit() == as<CSSUnitValue>(*values[0]).unit();
        return false;
    });
}

template<typename Operation>
static GC::Ref<CSSNumericValue> apply_math_operation_on_css_unit_values(ReadonlySpan<GC::Ref<CSSNumericValue>> values, Operation&& operation)
{
    auto& first_unit_value = as<CSSUnitValue>(*values[0]);
    auto& unit = first_unit_value.unit();

    double result = first_unit_value.value();
    for (size_t i = 1; i < values.size(); ++i)
        result = operation(result, as<CSSUnitValue>(*values[i]).value());
    return CSSUnitValue::create(result, unit);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-add
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::add(ReadonlySpan<CSSNumberish> initial_values)
{
    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathSum object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values;
    if (auto const* math_sum = as_if<CSSMathSum>(*this))
        values.extend(math_sum->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(value));

    // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose unit
    //    internal slot is set to that unit, and value internal slot is set to the sum of the value internal slots of
    //    the items in values. This addition must be done "left to right" - if values is « 1, 2, 3, 4 », the result must
    //    be (((1 + 2) + 3) + 4). (This detail is necessary to ensure interoperability in the presence of floating-point
    //    arithmetic.)
    if (all_values_are_css_unit_values_with_the_same_unit(values))
        return apply_math_operation_on_css_unit_values(values, [](double a, double b) { return a + b; });

    // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
    // 5. Return a new CSSMathSum object whose values internal slot is set to values.
    return TRY(CSSMathSum::add_all_types_into_math_sum(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-sub
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::sub(ReadonlySpan<CSSNumberish> initial_values)
{
    // 1. Replace each item of values with the result of rectifying a numberish value for the item, then negating the value.
    Vector<CSSNumberish> values;
    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(value)->negate());

    // 2. Return the result of calling the add() internal algorithm with this and values.
    return add(values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssmath-negate-a-cssnumericvalue
CSSNumberish CSSNumericValue::negate()
{
    // 1. If this is a CSSMathNegate object, return this’s value internal slot.
    if (auto* negate = as_if<CSSMathNegate>(*this))
        return GC::Ref<CSSNumericValue> { negate->value() };

    // 2. If this is a CSSUnitValue object, return a new CSSUnitValue with the same unit internal slot as this, and a
    //    value internal slot set to the negation of this’s.
    if (auto* unit_value = as_if<CSSUnitValue>(*this))
        return GC::Ref<CSSNumericValue> { CSSUnitValue::create(-unit_value->value(), unit_value->unit()) };

    // 3. Otherwise, return a new CSSMathNegate object whose value internal slot is set to this.
    return GC::Ref<CSSNumericValue> { CSSMathNegate::create_from_numberish(GC::Ref<CSSNumericValue> { *this }) };
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-mul
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::mul(ReadonlySpan<CSSNumberish> initial_values)
{
    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathProduct object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values;
    if (auto const* math_product = as_if<CSSMathProduct>(*this))
        values.extend(math_product->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(value));

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
            if (unit == "number"_utf16_fly_string)
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
            auto unit = non_number_unit_index.has_value() ? as<CSSUnitValue>(*values[*non_number_unit_index]).unit() : "number"_utf16_fly_string;
            return CSSUnitValue::create(product, unit);
        }
    }

    // 5. Let type be the result of multiplying the types of every item in values. If type is failure, throw a TypeError.
    // 6. Return a new CSSMathProduct object whose values internal slot is set to values.
    return TRY(CSSMathProduct::multiply_all_types_into_math_product(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-div
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::div(ReadonlySpan<CSSNumberish> initial_values)
{
    // 1. Replace each item of values with the result of rectifying a numberish value for the item, then inverting the value.
    Vector<CSSNumberish> values;
    for (auto const& value : initial_values)
        values.append(TRY(rectify_a_numberish_value(value)->invert()));

    // 2. Return the result of calling the mul() internal algorithm with this and values.
    return mul(values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssmath-invert-a-cssnumericvalue
WebIDL::ExceptionOr<CSSNumberish> CSSNumericValue::invert()
{
    // 1. If this is a CSSMathInvert object, return this’s value internal slot.
    if (auto* invert = as_if<CSSMathInvert>(*this))
        return CSSNumberish { GC::Ref<CSSNumericValue> { invert->value() } };

    // 2. If this is a CSSUnitValue object with unit internal slot set to "number":
    if (auto* unit_value = as_if<CSSUnitValue>(*this); unit_value && unit_value->unit() == "number"_utf16_fly_string) {
        // 1. If this’s value internal slot is set to 0 or -0, throw a RangeError.
        if (unit_value->value() == 0 || unit_value->value() == -0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Zero has no multiplicative inverse"_utf16 };

        // 2. Else return a new CSSUnitValue with the unit internal slot set to "number", and a value internal slot set
        //    to 1 divided by this’s {CSSUnitValue/value}} internal slot.
        return CSSNumberish { GC::Ref<CSSNumericValue> { CSSUnitValue::create(1.0 / unit_value->value(), "number"_utf16_fly_string) } };
    }

    // 3. Otherwise, return a new CSSMathInvert object whose value internal slot is set to this.
    return CSSNumberish { GC::Ref<CSSNumericValue> { CSSMathInvert::create_from_numberish(GC::Ref<CSSNumericValue> { *this }) } };
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-min
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::min(ReadonlySpan<CSSNumberish> initial_values)
{
    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathMin object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values;
    if (auto const* math_product = as_if<CSSMathMin>(*this))
        values.extend(math_product->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(value));

    // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose unit
    //    internal slot is set to that unit, and value internal slot is set to the minimum of the value internal slots
    //    of the items in values.
    if (all_values_are_css_unit_values_with_the_same_unit(values))
        return apply_math_operation_on_css_unit_values(values, [](double a, double b) { return AK::min(a, b); });

    // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
    // 5. Return a new CSSMathMin object whose values internal slot is set to values.
    return TRY(CSSMathMin::add_all_types_into_math_min(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-max
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::max(ReadonlySpan<CSSNumberish> initial_values)
{
    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. If this is a CSSMathMax object, prepend the items in this’s values internal slot to values.
    //    Otherwise, prepend this to values.

    // NB: We reorder the steps a little to avoid the awkward prepending.
    GC::RootVector<GC::Ref<CSSNumericValue>> values;
    if (auto const* math_product = as_if<CSSMathMax>(*this))
        values.extend(math_product->values()->values());
    else
        values.append(*this);

    for (auto const& value : initial_values)
        values.append(rectify_a_numberish_value(value));

    // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose unit
    //    internal slot is set to that unit, and value internal slot is set to the maximum of the value internal slots
    //    of the items in values.
    if (all_values_are_css_unit_values_with_the_same_unit(values))
        return apply_math_operation_on_css_unit_values(values, [](double a, double b) { return AK::max(a, b); });

    // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
    // 5. Return a new CSSMathMax object whose values internal slot is set to values.
    return TRY(CSSMathMax::add_all_types_into_math_max(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-equals
bool CSSNumericValue::equals_for_bindings(ReadonlySpan<CSSNumberish> values) const
{
    // The equals(...values) method, when called on a CSSNumericValue this, must perform the following steps:

    // 1. Replace each item of values with the result of rectifying a numberish value for the item.
    // 2. For each item in values, if the item is not an equal numeric value to this, return false.
    for (auto const& value : values) {
        auto rectified_value = rectify_a_numberish_value(value);
        if (!is_equal_numeric_value(rectified_value))
            return false;
    }

    // 3. Return true.
    return true;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-to
WebIDL::ExceptionOr<GC::Ref<CSSUnitValue>> CSSNumericValue::to(Utf16String const& unit) const
{
    return to(Utf16FlyString { unit });
}

WebIDL::ExceptionOr<GC::Ref<CSSUnitValue>> CSSNumericValue::to(Utf16FlyString const& unit) const
{
    // The to(unit) method converts an existing CSSNumericValue this into another one with the specified unit, if
    // possible. When called, it must perform the following steps:

    // 1. Let type be the result of creating a type from unit. If type is failure, throw a SyntaxError.
    auto maybe_type = NumericType::create_from_unit(unit);
    if (!maybe_type.has_value())
        return WebIDL::SyntaxError::create(Utf16String::formatted("Unrecognized unit '{}'", unit));

    // 2. Let sum be the result of creating a sum value from this. If sum is failure, throw a TypeError.
    auto sum = create_a_sum_value();
    if (!sum.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Unable to create a sum from input '{}'", MUST(to_string())) };

    // 3. If sum has more than one item, throw a TypeError.
    //    Otherwise, let item be the result of creating a CSSUnitValue from the sole item in sum, then converting it to
    //    unit. If item is failure, throw a TypeError.
    if (sum->size() > 1)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Sum contains more than one item"_utf16 };
    auto item = CSSUnitValue::create_from_sum_value_item(sum->first());
    if (!item)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Unable to create CSSUnitValue from input '{}'", MUST(to_string())) };

    auto converted_item = item->converted_to_unit(unit);
    if (!converted_item)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unable to convert input to requested unit"_utf16 };

    // 4. Return item.
    return converted_item.as_nonnull();
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssnumericvalue
void CSSNumericValue::serialize(Utf16StringBuilder& builder, SerializationParams const& params) const
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

Utf16String CSSNumericValue::to_string(SerializationParams const& params) const
{
    Utf16StringBuilder builder;
    serialize(builder, params);
    return builder.to_string();
}

// https://drafts.css-houdini.org/css-typed-om-1/#rectify-a-numberish-value
GC::Ref<CSSNumericValue> rectify_a_numberish_value(CSSNumberish const& numberish, Optional<Utf16FlyString> unit)
{
    return numberish.visit(
        [](GC::Ref<CSSNumericValue> num) -> GC::Ref<CSSNumericValue> { return num; },
        [&unit](double num) -> GC::Ref<CSSNumericValue> {
            return CSSUnitValue::create(num, unit.value_or("number"_utf16_fly_string));
        });
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-numeric-value
static WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> reify_a_numeric_value(StyleValue const& numeric_value)
{
    if (numeric_value.is_calculated()) {
        auto reified = numeric_value.as_calculated().reify({});
        if (auto* reified_numeric = as_if<CSSNumericValue>(*reified))
            return GC::Ref { *reified_numeric };
        return WebIDL::SyntaxError::create("Unable to reify this math function."_utf16);
    }
    if (numeric_value.is_number())
        return CSSUnitValue::create(numeric_value.as_number().number(), "number"_utf16_fly_string);
    VERIFY(numeric_value.is_dimension());
    return CSSUnitValue::create(numeric_value.as_dimension().raw_value(), numeric_value.as_dimension().unit_name());
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-parse
WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> CSSNumericValue::parse(JS::VM& vm, Utf16View css_text)
{
    (void)vm;
    // The parse(cssText) method, when called, must perform the following steps:

    for (auto value_type : { ValueType::Number, ValueType::Percentage, ValueType::Length, ValueType::LengthPercentage, ValueType::Angle, ValueType::Time, ValueType::Frequency, ValueType::Resolution, ValueType::Flex }) {
        auto value = parse_css_type(Parser::ParsingParams {}, css_text, value_type);
        if (value)
            return reify_a_numeric_value(*value);
    }
    return WebIDL::SyntaxError::create("Input is not a numeric component value."_utf16);
}

}
