/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathValue.h"
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathValue);

CSSMathValue::CSSMathValue(JS::Realm& realm, Bindings::CSSMathOperator operator_, NumericType type)
    : CSSNumericValue(realm, move(type))
    , m_operator(operator_)
{
}

void CSSMathValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathValue);
    Base::initialize(realm);
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> CSSMathValue::create_an_internal_representation(PropertyNameAndID const& property, PerformTypeCheck perform_type_check) const
{
    // If value is a CSSStyleValue subclass,
    //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
    //
    //     If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of value
    //     is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it in a
    //     fresh CSSMathSum whose values internal slot contains only that part of value.
    //
    //     Return the value.

    // FIXME: Check types allowed by registered custom properties.
    auto context = CalculationContext::for_property(property);
    auto matches = [&] {
        if (type().matches_angle(context.percentages_resolve_as))
            return property_accepts_type(property.id(), ValueType::Angle);
        if (type().matches_flex(context.percentages_resolve_as))
            return property_accepts_type(property.id(), ValueType::Flex);
        if (type().matches_frequency(context.percentages_resolve_as))
            return property_accepts_type(property.id(), ValueType::Frequency);
        if (type().matches_length(context.percentages_resolve_as))
            return property_accepts_type(property.id(), ValueType::Length);
        if (type().matches_number(context.percentages_resolve_as))
            return property_accepts_type(property.id(), ValueType::Number);
        if (type().matches_percentage())
            return property_accepts_type(property.id(), ValueType::Percentage);
        if (type().matches_resolution(context.percentages_resolve_as))
            return property_accepts_type(property.id(), ValueType::Resolution);
        if (type().matches_time(context.percentages_resolve_as))
            return property_accepts_type(property.id(), ValueType::Time);
        return false;
    }();

    if (perform_type_check == PerformTypeCheck::Yes && !matches)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Property does not accept values of this type."sv };

    return CalculatedStyleValue::create(TRY(create_calculation_node(context)), type(), move(context));
}

}
