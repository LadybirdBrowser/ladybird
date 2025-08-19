/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathMax.h"
#include <LibWeb/Bindings/CSSMathMaxPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSMathNegate.h>
#include <LibWeb/CSS/CSSNumericArray.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathMax);

GC::Ref<CSSMathMax> CSSMathMax::create(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericArray> values)
{
    return realm.create<CSSMathMax>(realm, move(type), move(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathmin-cssmathmin
WebIDL::ExceptionOr<GC::Ref<CSSMathMax>> CSSMathMax::construct_impl(JS::Realm& realm, Vector<CSSNumberish> values)
{
    // The CSSMathMin(...args) and CSSMathMax(...args) constructors are defined identically to the above, except that
    // in the last step they return a new CSSMathMin or CSSMathMax object, respectively.
    // NB: So, the steps below are a modification of the CSSMathSum steps.

    // 1. Replace each item of args with the result of rectifying a numberish value for the item.
    GC::RootVector<GC::Ref<CSSNumericValue>> converted_values { realm.heap() };
    converted_values.ensure_capacity(values.size());
    for (auto const& value : values) {
        converted_values.append(rectify_a_numberish_value(realm, value));
    }

    // 2. If args is empty, throw a SyntaxError.
    if (converted_values.is_empty())
        return WebIDL::SyntaxError::create(realm, "Cannot create an empty CSSMathMax"_utf16);

    // 3. Let type be the result of adding the types of all the items of args. If type is failure, throw a TypeError.
    auto type = converted_values.first()->type();
    bool first = true;
    for (auto const& value : converted_values) {
        if (first) {
            first = false;
            continue;
        }
        if (auto added_types = type.added_to(value->type()); added_types.has_value()) {
            type = added_types.release_value();
        } else {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create a CSSMathMax with values of incompatible types"sv };
        }
    }

    // 4. Return a new CSSMathMax whose values internal slot is set to args.
    auto values_array = CSSNumericArray::create(realm, { converted_values });
    return CSSMathMax::create(realm, move(type), move(values_array));
}

CSSMathMax::CSSMathMax(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericArray> values)
    : CSSMathValue(realm, Bindings::CSSMathOperator::Max, move(type))
    , m_values(move(values))
{
}

CSSMathMax::~CSSMathMax() = default;

void CSSMathMax::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathMax);
    Base::initialize(realm);
}

void CSSMathMax::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
String CSSMathMax::serialize_math_value(Nested, Parens) const
{
    // NB: Only steps 1 and 2 apply here.
    // 1. Let s initially be the empty string.
    StringBuilder s;

    // 2. If this is a CSSMathMin or CSSMathMax:
    {
        // 1. Append "min(" or "max(" to s, as appropriate.
        s.append("max("sv);

        // 2. For each arg in this’s values internal slot, serialize arg with nested and paren-less both true, and
        //    append the result to s, appending a ", " between successive values.
        bool first = true;
        for (auto const& arg : m_values->values()) {
            if (first) {
                first = false;
            } else {
                s.append(", "sv);
            }
            s.append(arg->to_string({ .nested = true, .parenless = true }));
        }

        // 3. Append ")" to s and return s.
        s.append(")"sv);
        return s.to_string_without_validation();
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathmin-values
GC::Ref<CSSNumericArray> CSSMathMax::values() const
{
    return m_values;
}

}
