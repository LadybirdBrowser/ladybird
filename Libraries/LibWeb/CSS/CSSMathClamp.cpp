/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathClamp.h"
#include <LibWeb/Bindings/CSSMathClampPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSMathNegate.h>
#include <LibWeb/CSS/CSSNumericArray.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathClamp);

GC::Ref<CSSMathClamp> CSSMathClamp::create(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericValue> lower, GC::Ref<CSSNumericValue> value, GC::Ref<CSSNumericValue> upper)
{
    return realm.create<CSSMathClamp>(realm, move(type), move(lower), move(value), move(upper));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-cssmathclamp
WebIDL::ExceptionOr<GC::Ref<CSSMathClamp>> CSSMathClamp::construct_impl(JS::Realm& realm, CSSNumberish lower, CSSNumberish value, CSSNumberish upper)
{
    // The CSSMathClamp(lower, value, upper) constructor must, when called, perform the following steps:
    // 1. Replace lower, value, and upper with the result of rectifying a numberish value for each.
    auto lower_rectified = rectify_a_numberish_value(realm, lower);
    auto value_rectified = rectify_a_numberish_value(realm, value);
    auto upper_rectified = rectify_a_numberish_value(realm, upper);

    // 2. Let type be the result of adding the types of lower, value, and upper. If type is failure, throw a TypeError.
    auto type = lower_rectified->type()
                    .added_to(value_rectified->type())
                    .map([&](auto& type) { return type.added_to(upper_rectified->type()); });
    if (!type.has_value()) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create a CSSMathClamp with values of incompatible types"sv };
    }

    // 3. Return a new CSSMathClamp whose lower, value, and upper internal slots are set to lower, value, and upper, respectively.
    return CSSMathClamp::create(realm, type->release_value(), move(lower_rectified), move(value_rectified), move(upper_rectified));
}

CSSMathClamp::CSSMathClamp(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericValue> lower, GC::Ref<CSSNumericValue> value, GC::Ref<CSSNumericValue> upper)
    : CSSMathValue(realm, Bindings::CSSMathOperator::Clamp, move(type))
    , m_lower(move(lower))
    , m_value(move(value))
    , m_upper(move(upper))
{
}

CSSMathClamp::~CSSMathClamp() = default;

void CSSMathClamp::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathClamp);
    Base::initialize(realm);
}

void CSSMathClamp::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_lower);
    visitor.visit(m_value);
    visitor.visit(m_upper);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
String CSSMathClamp::serialize_math_value(Nested, Parens) const
{
    // AD-HOC: The spec is missing serialization rules for CSSMathClamp: https://github.com/w3c/css-houdini-drafts/issues/1152
    StringBuilder s;
    s.append("clamp("sv);
    s.append(m_lower->to_string({ .nested = true, .parenless = true }));
    s.append(", "sv);
    s.append(m_value->to_string({ .nested = true, .parenless = true }));
    s.append(", "sv);
    s.append(m_upper->to_string({ .nested = true, .parenless = true }));
    s.append(")"sv);
    return s.to_string_without_validation();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-lower
GC::Ref<CSSNumericValue> CSSMathClamp::lower() const
{
    // AD-HOC: No spec definition.
    return m_lower;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-value
GC::Ref<CSSNumericValue> CSSMathClamp::value() const
{
    // AD-HOC: No spec definition.
    return m_value;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-upper
GC::Ref<CSSNumericValue> CSSMathClamp::upper() const
{
    // AD-HOC: No spec definition.
    return m_upper;
}

// https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
bool CSSMathClamp::is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const
{
    // AD-HOC: Spec doesn't handle clamp()
    // 1. If value1 and value2 are not members of the same interface, return false.
    auto* other_clamp = as_if<CSSMathClamp>(*other);
    if (!other_clamp)
        return false;

    return m_lower->is_equal_numeric_value(other_clamp->m_lower)
        && m_value->is_equal_numeric_value(other_clamp->m_value)
        && m_upper->is_equal_numeric_value(other_clamp->m_upper);
}

}
