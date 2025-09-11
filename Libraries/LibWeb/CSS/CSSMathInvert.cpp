/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathInvert.h"
#include <LibWeb/Bindings/CSSMathInvertPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathInvert);

GC::Ref<CSSMathInvert> CSSMathInvert::create(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericValue> values)
{
    return realm.create<CSSMathInvert>(realm, move(type), move(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathinvert-cssmathinvert
WebIDL::ExceptionOr<GC::Ref<CSSMathInvert>> CSSMathInvert::construct_impl(JS::Realm& realm, CSSNumberish value)
{
    // The CSSMathInvert(arg) constructor is defined identically to the above, except that in the last step it returns
    // a new CSSMathInvert object.
    // NB: So, the steps below are a modification of the CSSMathNegate steps.

    // 1. Replace arg with the result of rectifying a numberish value for arg.
    auto converted_value = rectify_a_numberish_value(realm, value);

    // 2. Return a new CSSMathInvert whose value internal slot is set to arg.
    return CSSMathInvert::create(realm, converted_value->type().inverted(), converted_value);
}

CSSMathInvert::CSSMathInvert(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericValue> values)
    : CSSMathValue(realm, Bindings::CSSMathOperator::Invert, move(type))
    , m_value(move(values))
{
}

CSSMathInvert::~CSSMathInvert() = default;

void CSSMathInvert::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathInvert);
    Base::initialize(realm);
}

void CSSMathInvert::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_value);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
String CSSMathInvert::serialize_math_value(Nested nested, Parens parens) const
{
    // NB: Only steps 1 and 6 apply here.
    // 1. Let s initially be the empty string.
    StringBuilder s;

    // 6. Otherwise, if this is a CSSMathInvert:
    {
        // 1. If paren-less is true, continue to the next step; otherwise, if nested is true, append "(" to s;
        //    otherwise, append "calc(" to s.
        if (parens == Parens::With) {
            if (nested == Nested::Yes) {
                s.append("("sv);
            } else {
                s.append("calc("sv);
            }
        }

        // 2. Append "1 / " to s.
        s.append("1 / "sv);

        // 3. Serialize this’s value internal slot with nested set to true, and append the result to s.
        s.append(m_value->to_string({ .nested = true }));

        // 4. If paren-less is false, append ")" to s,
        if (parens == Parens::With)
            s.append(")"sv);

        // 5. Return s.
        return s.to_string_without_validation();
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathinvert-value
GC::Ref<CSSNumericValue> CSSMathInvert::value() const
{
    return m_value;
}

// https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
bool CSSMathInvert::is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const
{
    // NB: Only steps 1, 4 and 5 are relevant.
    // 1. If value1 and value2 are not members of the same interface, return false.
    auto* other_invert = as_if<CSSMathInvert>(*other);
    if (!other_invert)
        return false;

    // 4. Assert: value1 and value2 are both CSSMathNegates or CSSMathInverts.
    // 5. Return whether value1’s value and value2’s value are equal numeric values.
    return m_value->is_equal_numeric_value(other_invert->m_value);
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
Optional<SumValue> CSSMathInvert::create_a_sum_value() const
{
    // 1. Let values be the result of creating a sum value from this’s value internal slot.
    auto values = m_value->create_a_sum_value();

    // 2. If values is failure, return failure.
    if (!values.has_value())
        return {};

    // 3. If the length of values is more than one, return failure.
    if (values->size() > 1)
        return {};

    // 4. Invert (find the reciprocal of) the value of the item in values, and negate the value of each entry in its unit map.
    for (auto& [value, unit_map] : *values) {
        value = 1.0 / value;
        for (auto& [_, power] : unit_map)
            power = -power;
    }

    // 5. Return values.
    return values;
}

}
