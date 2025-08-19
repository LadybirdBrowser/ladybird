/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathProduct.h"
#include <LibWeb/Bindings/CSSMathProductPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericArray.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathProduct);

GC::Ref<CSSMathProduct> CSSMathProduct::create(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericArray> values)
{
    return realm.create<CSSMathProduct>(realm, move(type), move(values));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathproduct-cssmathproduct
WebIDL::ExceptionOr<GC::Ref<CSSMathProduct>> CSSMathProduct::construct_impl(JS::Realm& realm, Vector<CSSNumberish> values)
{
    // The CSSMathProduct(...args) constructor is defined identically to the above, except that in step 3 it multiplies
    // the types instead of adding, and in the last step it returns a CSSMathProduct.
    // NB: So, the steps below are a modification of the CSSMathSum steps.

    // 1. Replace each item of args with the result of rectifying a numberish value for the item.
    GC::RootVector<GC::Ref<CSSNumericValue>> converted_values { realm.heap() };
    converted_values.ensure_capacity(values.size());
    for (auto const& value : values) {
        converted_values.append(rectify_a_numberish_value(realm, value));
    }

    // 2. If args is empty, throw a SyntaxError.
    if (converted_values.is_empty())
        return WebIDL::SyntaxError::create(realm, "Cannot create an empty CSSMathProduct"_utf16);

    // 3. Let type be the result of multiplying the types of all the items of args. If type is failure, throw a TypeError.
    auto type = converted_values.first()->type();
    bool first = true;
    for (auto const& value : converted_values) {
        if (first) {
            first = false;
            continue;
        }
        if (auto multiplied_types = type.multiplied_by(value->type()); multiplied_types.has_value()) {
            type = multiplied_types.release_value();
        } else {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create a CSSMathProduct with values of incompatible types"sv };
        }
    }

    // 4. Return a new CSSMathProduct whose values internal slot is set to args.
    auto values_array = CSSNumericArray::create(realm, { converted_values });
    return CSSMathProduct::create(realm, move(type), move(values_array));
}

CSSMathProduct::CSSMathProduct(JS::Realm& realm, NumericType type, GC::Ref<CSSNumericArray> values)
    : CSSMathValue(realm, Bindings::CSSMathOperator::Product, move(type))
    , m_values(move(values))
{
}

CSSMathProduct::~CSSMathProduct() = default;

void CSSMathProduct::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathProduct);
    Base::initialize(realm);
}

void CSSMathProduct::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
String CSSMathProduct::serialize_math_value(Nested nested, Parens parens) const
{
    // NB: Only steps 1 and 5 apply here.
    // 1. Let s initially be the empty string.
    StringBuilder s;

    // 5. Otherwise, if this is a CSSMathProduct:
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

        // 2. Serialize the first item in this’s values internal slot with nested set to true, and append the result to s.
        s.append(m_values->values().first()->to_string({ .nested = true }));

        // 3. For each arg in this’s values internal slot beyond the first:
        bool first = true;
        for (auto const& arg : m_values->values()) {
            if (first) {
                first = false;
                continue;
            }

            // 1. If arg is a CSSMathInvert, append " / " to s, then serialize arg’s value internal slot with nested
            //    set to true, and append the result to s.
            // FIXME: Detect CSSMathInvert once that's implemented.
            if (false) {
                s.append(" / "sv);
                s.append(arg->to_string({ .nested = true }));
            }

            // 2. Otherwise, append " * " to s, then serialize arg with nested set to true, and append the result to s.
            else {
                s.append(" * "sv);
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

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathproduct-values
GC::Ref<CSSNumericArray> CSSMathProduct::values() const
{
    return m_values;
}

}
