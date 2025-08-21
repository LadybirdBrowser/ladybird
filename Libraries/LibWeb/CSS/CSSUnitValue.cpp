/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSUnitValue.h"
#include <LibWeb/Bindings/CSSUnitValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSUnitValue);

GC::Ref<CSSUnitValue> CSSUnitValue::create(JS::Realm& realm, double value, FlyString unit)
{
    // The type of a CSSUnitValue is the result of creating a type from its unit internal slot.
    // https://drafts.css-houdini.org/css-typed-om-1/#type-of-a-cssunitvalue
    auto numeric_type = NumericType::create_from_unit(unit);
    return realm.create<CSSUnitValue>(realm, value, move(unit), numeric_type.release_value());
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-cssunitvalue
WebIDL::ExceptionOr<GC::Ref<CSSUnitValue>> CSSUnitValue::construct_impl(JS::Realm& realm, double value, FlyString unit)
{
    // 1. If creating a type from unit returns failure, throw a TypeError and abort this algorithm.
    auto numeric_type = NumericType::create_from_unit(unit);
    if (!numeric_type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Cannot create CSSUnitValue with unrecognized unit '{}'", unit)) };

    // 2. Return a new CSSUnitValue with its value internal slot set to value and its unit set to unit.
    return realm.create<CSSUnitValue>(realm, value, move(unit), numeric_type.release_value());
}

CSSUnitValue::CSSUnitValue(JS::Realm& realm, double value, FlyString unit, NumericType type)
    : CSSNumericValue(realm, move(type))
    , m_value(value)
    // AD-HOC: WPT expects the unit to be lowercase but this doesn't seem to be specified anywhere.
    , m_unit(unit.to_ascii_lowercase())
{
}

void CSSUnitValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSUnitValue);
    Base::initialize(realm);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-value
void CSSUnitValue::set_value(double value)
{
    // AD-HOC: No definition: https://github.com/w3c/css-houdini-drafts/issues/1146
    m_value = value;
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssunitvalue
String CSSUnitValue::serialize_unit_value(Optional<double> minimum, Optional<double> maximum) const
{
    // To serialize a CSSUnitValue this, with optional arguments minimum, a numeric value, and maximum, a numeric value:

    // 1. Let value and unit be this‘s value and unit internal slots.

    // 2. Set s to the result of serializing a <number> from value, per CSSOM §6.7.2 Serializing CSS Values.
    StringBuilder s;
    serialize_a_number(s, m_value);

    // 3. If unit is:
    // -> "number"
    if (m_unit == "number"_fly_string) {
        // Do nothing.
    }
    // -> "percent"
    else if (m_unit == "percent"_fly_string) {
        // Append "%" to s.
        s.append("%"sv);
    }
    // -> anything else
    else {
        // Append unit to s.
        s.append(m_unit.to_ascii_lowercase());
    }

    // 4. If minimum was passed and this is less than minimum, or if maximum was passed and this is greater than
    //    maximum, or either minimum and/or maximum were passed and the relative size of this and minimum/maximum can’t
    //    be determined with the available information at this time, prepend "calc(" to s, then append ")" to s.
    if ((minimum.has_value() && m_value < minimum.value())
        || (maximum.has_value() && m_value > maximum.value())) {
        // FIXME: "or either minimum and/or maximum were passed and the relative size of this and minimum/maximum can’t be determined with the available information at this time"
        return MUST(String::formatted("calc({})", s.string_view()));
    }

    // 5. Return s.
    return s.to_string_without_validation();
}

}
