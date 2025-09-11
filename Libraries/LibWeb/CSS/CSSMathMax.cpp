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

// https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
bool CSSMathMax::is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const
{
    // NB: Only steps 1 and 3 are relevant.
    // 1. If value1 and value2 are not members of the same interface, return false.
    auto* other_max = as_if<CSSMathMax>(*other);
    if (!other_max)
        return false;

    // 3. If value1 and value2 are both CSSMathSums, CSSMathProducts, CSSMathMins, or CSSMathMaxs:
    // NB: Substeps are implemented in CSSNumericArray.
    return m_values->is_equal_numeric_values(other_max->m_values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
Optional<SumValue> CSSMathMax::create_a_sum_value() const
{
    // 1. Let args be the result of creating a sum value for each item in this’s values internal slot.
    Vector<Optional<SumValue>> args;
    args.ensure_capacity(m_values->length());
    for (auto const& value : m_values->values()) {
        args.unchecked_append(value->create_a_sum_value());
    }

    Optional<SumValue> item_with_largest_value = args.first();
    for (auto const& item : args) {
        // 2. If any item of args is failure, or has a length greater than one, return failure.
        if (!item.has_value() || item->size() > 1)
            return {};

        // 3. If not all of the unit maps among the items of args are identical, return failure.
        if (item->first().unit_map != item_with_largest_value->first().unit_map)
            return {};

        if (item->first().value > item_with_largest_value->first().value)
            item_with_largest_value = item;
    }

    // 4. Return the item of args whose sole item has the largest value.
    return item_with_largest_value;
}

}
