/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathSum.h"
#include <LibWeb/Bindings/CSSMathSumPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericArray.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathSum);

GC::Ref<CSSMathSum> CSSMathSum::create(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericArray> values)
{
    return realm.create<CSSMathSum>(realm, move(type), move(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathsum-cssmathsum
WebIDL::ExceptionOr<GC::Ref<CSSMathSum>> CSSMathSum::construct_impl(JS::Realm& realm, Vector<CSSNumberish> values)
{
    // The CSSMathSum(...args) constructor must, when called, perform the following steps:

    // 1. Replace each item of args with the result of rectifying a numberish value for the item.
    GC::RootVector<GC::Ref<CSSNumericValue>> converted_values { realm.heap() };
    converted_values.ensure_capacity(values.size());
    for (auto const& value : values) {
        converted_values.append(rectify_a_numberish_value(realm, value));
    }

    // 2. If args is empty, throw a SyntaxError.
    if (converted_values.is_empty())
        return WebIDL::SyntaxError::create(realm, "Cannot create an empty CSSMathSum"_utf16);

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
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create a CSSMathSum with values of incompatible types"sv };
        }
    }

    // 4. Return a new CSSMathSum whose values internal slot is set to args.
    auto values_array = CSSNumericArray::create(realm, { converted_values });
    return CSSMathSum::create(realm, move(type), move(values_array));
}

CSSMathSum::CSSMathSum(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericArray> values)
    : CSSMathValue(realm, Bindings::CSSMathOperator::Sum, move(type))
    , m_values(move(values))
{
}

CSSMathSum::~CSSMathSum() = default;

void CSSMathSum::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathSum);
    Base::initialize(realm);
}

void CSSMathSum::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
String CSSMathSum::serialize_math_value(Nested nested, Parens parens) const
{
    // NB: Only steps 1 and 3 apply here.
    // 1. Let s initially be the empty string.
    StringBuilder s;

    // 3. Otherwise, if this is a CSSMathSum:
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

        // 2. Serialize the first item in this’s values internal slot with nested set to true, and append the result
        //    to s.
        s.append(m_values->values().first()->to_string({ .nested = true }));

        // 3. For each arg in this’s values internal slot beyond the first:
        bool first = true;
        for (auto const& arg : m_values->values()) {
            if (first) {
                first = false;
                continue;
            }

            // 1. If arg is a CSSMathNegate, append " - " to s, then serialize arg’s value internal slot with nested
            //    set to true, and append the result to s.
            // FIXME: Detect CSSMathNegate once that's implemented.
            if (false) {
                s.append(" - "sv);
                s.append(arg->to_string({ .nested = true }));
            }

            // 2. Otherwise, append " + " to s, then serialize arg with nested set to true, and append the result to s.
            else {
                s.append(" + "sv);
                s.append(arg->to_string({ .nested = true }));
            }
        }

        // 4. If paren-less is false, append ")" to s,
        if (parens == Parens::With)
            s.append(")"sv);

        // 5. Return s.
        return s.to_string_without_validation();
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathsum-values
GC::Ref<CSSNumericArray> CSSMathSum::values() const
{
    return m_values;
}

}
