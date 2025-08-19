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
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/Serialize.h>

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

}
