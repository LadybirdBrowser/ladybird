/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathNegate.h"
#include <LibWeb/Bindings/CSSMathNegatePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathNegate);

GC::Ref<CSSMathNegate> CSSMathNegate::create(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericValue> values)
{
    return realm.create<CSSMathNegate>(realm, move(type), move(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathnegate-cssmathnegate
WebIDL::ExceptionOr<GC::Ref<CSSMathNegate>> CSSMathNegate::construct_impl(JS::Realm& realm, CSSNumberish value)
{
    // The CSSMathNegate(arg) constructor must, when called, perform the following steps:
    // 1. Replace arg with the result of rectifying a numberish value for arg.
    auto converted_value = rectify_a_numberish_value(realm, value);

    // 2. Return a new CSSMathNegate whose value internal slot is set to arg.
    return CSSMathNegate::create(realm, converted_value->type(), converted_value);
}

CSSMathNegate::CSSMathNegate(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericValue> values)
    : CSSMathValue(realm, Bindings::CSSMathOperator::Negate, move(type))
    , m_value(move(values))
{
}

CSSMathNegate::~CSSMathNegate() = default;

void CSSMathNegate::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathNegate);
    Base::initialize(realm);
}

void CSSMathNegate::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_value);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
String CSSMathNegate::serialize_math_value(Nested nested, Parens parens) const
{
    // NB: Only steps 1 and 4 apply here.
    // 1. Let s initially be the empty string.
    StringBuilder s;

    // 4. Otherwise, if this is a CSSMathNegate:
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

        // 2. Append "-" to s.
        s.append("-"sv);

        // 3. Serialize this’s value internal slot with nested set to true, and append the result to s.
        s.append(m_value->to_string({ .nested = true }));

        // 4. If paren-less is false, append ")" to s,
        if (parens == Parens::With)
            s.append(")"sv);

        // 5. Return s.
        return s.to_string_without_validation();
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathnegate-value
GC::Ref<CSSNumericValue> CSSMathNegate::value() const
{
    return m_value;
}

// https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
bool CSSMathNegate::is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const
{
    // NB: Only steps 1, 4 and 5 are relevant.
    // 1. If value1 and value2 are not members of the same interface, return false.
    auto* other_negate = as_if<CSSMathNegate>(*other);
    if (!other_negate)
        return false;

    // 4. Assert: value1 and value2 are both CSSMathNegates or CSSMathInverts.
    // 5. Return whether value1’s value and value2’s value are equal numeric values.
    return m_value->is_equal_numeric_value(other_negate->m_value);
}

}
