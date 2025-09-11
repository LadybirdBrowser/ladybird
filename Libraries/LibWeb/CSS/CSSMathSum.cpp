/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathSum.h"
#include <LibWeb/Bindings/CSSMathSumPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSMathNegate.h>
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
            if (auto* negate = as_if<CSSMathNegate>(*arg)) {
                s.append(" - "sv);
                s.append(negate->value()->to_string({ .nested = true }));
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

// https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
bool CSSMathSum::is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const
{
    // NB: Only steps 1 and 3 are relevant.
    // 1. If value1 and value2 are not members of the same interface, return false.
    auto* other_sum = as_if<CSSMathSum>(*other);
    if (!other_sum)
        return false;

    // 3. If value1 and value2 are both CSSMathSums, CSSMathProducts, CSSMathMins, or CSSMathMaxs:
    // NB: Substeps are implemented in CSSNumericArray.
    return m_values->is_equal_numeric_values(other_sum->m_values);
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
Optional<SumValue> CSSMathSum::create_a_sum_value() const
{
    // 1. Let values initially be an empty list.
    SumValue values;

    // 2. For each item in this’s values internal slot:
    for (auto const item : m_values->values()) {
        // 1. Let value be the result of creating a sum value from item. If value is failure, return failure.
        auto maybe_value = item->create_a_sum_value();
        if (!maybe_value.has_value())
            return {};
        auto const& value = maybe_value.value();

        // 2. For each subvalue of value:
        for (auto const& subvalue : value) {
            // 1. If values already contains an item with the same unit map as subvalue, increment that item’s value by
            //    the value of subvalue.
            auto existing_item = values.find_if([&subvalue](SumValueItem const& other) {
                return subvalue.unit_map == other.unit_map;
            });
            if (existing_item != values.end()) {
                existing_item->value += subvalue.value;
            }
            // 2. Otherwise, append subvalue to values.
            else {
                values.append(subvalue);
            }
        }
    }

    // 3. Create a type from the unit map of each item of values, and add all the types together.
    //    If the result is failure, return failure.
    auto added_type = NumericType::create_from_unit_map(values.first().unit_map);
    if (!added_type.has_value())
        return {};
    bool first = true;
    for (auto const& [_, unit_map] : values) {
        if (first) {
            first = false;
            continue;
        }
        auto type = NumericType::create_from_unit_map(unit_map);
        if (!type.has_value())
            return {};

        added_type = added_type->added_to(type.value());
        if (!added_type.has_value())
            return {};
    }

    // 4. Return values.
    return values;
}

}
