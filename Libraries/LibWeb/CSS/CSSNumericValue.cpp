/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNumericValue.h"
#include <LibWeb/Bindings/CSSNumericValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
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
    // FIXME: 2. Otherwise, serialize a CSSMathValue from this, and return the result.

    return {};
}

}
