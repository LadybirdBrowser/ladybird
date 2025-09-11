/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSUnitValue.h"
#include <LibWeb/Bindings/CSSUnitValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/Units.h>
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

// https://drafts.css-houdini.org/css-typed-om-1/#create-a-cssunitvalue-from-a-sum-value-item
GC::Ptr<CSSUnitValue> CSSUnitValue::create_from_sum_value_item(JS::Realm& realm, SumValueItem const& item)
{
    // 1. If item has more than one entry in its unit map, return failure.
    if (item.unit_map.size() > 1)
        return {};

    // 2. If item has no entries in its unit map, return a new CSSUnitValue whose unit internal slot is set to
    //    "number", and whose value internal slot is set to item’s value.
    if (item.unit_map.is_empty())
        return CSSUnitValue::create(realm, item.value, "number"_fly_string);

    // 3. Otherwise, item has a single entry in its unit map. If that entry’s value is anything other than 1, return
    //    failure.
    auto single_type_entry = item.unit_map.begin();
    if (single_type_entry->value != 1)
        return {};

    // 4. Otherwise, return a new CSSUnitValue whose unit internal slot is set to that entry’s key, and whose value
    //    internal slot is set to item’s value.
    return CSSUnitValue::create(realm, item.value, single_type_entry->key);
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

// https://drafts.css-houdini.org/css-typed-om-1/#convert-a-cssunitvalue
GC::Ptr<CSSUnitValue> CSSUnitValue::converted_to_unit(FlyString const& unit) const
{
    // 1. Let old unit be the value of this’s unit internal slot, and old value be the value of this’s value internal
    //    slot.
    auto old_unit = m_unit;
    auto old_value = m_value;

    // 2. If old unit and unit are not compatible units, return failure.
    double ratio = 1.0;
    // NB: If the units are identical, they're always compatible. That also covers cases of `number` and `percent`
    //     which aren't actually units.
    if (old_unit != unit) {
        auto old_dimension_type = dimension_for_unit(old_unit);
        auto new_dimension_type = dimension_for_unit(unit);
        if (!new_dimension_type.has_value() || old_dimension_type != new_dimension_type)
            return {};

        switch (*new_dimension_type) {
        case DimensionType::Angle: {
            auto from = string_to_angle_unit(old_unit).release_value();
            auto to = string_to_angle_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return {};
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Flex: {
            auto from = string_to_angle_unit(old_unit).release_value();
            auto to = string_to_angle_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return {};
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Frequency: {
            auto from = string_to_frequency_unit(old_unit).release_value();
            auto to = string_to_frequency_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return {};
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Length: {
            auto from = string_to_length_unit(old_unit).release_value();
            auto to = string_to_length_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return {};
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Resolution: {
            auto from = string_to_resolution_unit(old_unit).release_value();
            auto to = string_to_resolution_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return {};
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Time: {
            auto from = string_to_time_unit(old_unit).release_value();
            auto to = string_to_time_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return {};
            ratio = ratio_between_units(from, to);
            break;
        }
        }
    }

    // 3. Return a new CSSUnitValue whose unit internal slot is set to unit, and whose value internal slot is set to
    //    old value multiplied by the conversation ratio between old unit and unit.
    return CSSUnitValue::create(realm(), old_value * ratio, unit);
}

// https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
bool CSSUnitValue::is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const
{
    // NB: Only steps 1 and 2 are relevant.
    // 1. If value1 and value2 are not members of the same interface, return false.
    auto* other_unit_value = as_if<CSSUnitValue>(*other);
    if (!other_unit_value)
        return false;

    // 2. If value1 and value2 are both CSSUnitValues, return true if they have equal unit and value internal slots,
    //    or false otherwise.
    return m_unit == other_unit_value->m_unit
        && m_value == other_unit_value->m_value;
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
Optional<SumValue> CSSUnitValue::create_a_sum_value() const
{
    // 1. Let unit be the value of this’s unit internal slot, and value be the value of this’s value internal slot.
    auto unit = m_unit;
    auto value = m_value;

    // 2. If unit is a member of a set of compatible units, and is not the set’s canonical unit, multiply value
    //    by the conversion ratio between unit and the canonical unit, and change unit to the canonical unit.
    if (auto dimension_type = dimension_for_unit(unit); dimension_type.has_value()) {
        switch (*dimension_type) {
        case DimensionType::Angle: {
            auto angle_unit = string_to_angle_unit(unit).release_value();
            auto canonical_unit = canonical_angle_unit();
            if (angle_unit != canonical_unit && units_are_compatible(angle_unit, canonical_unit)) {
                value *= ratio_between_units(angle_unit, canonical_unit);
                unit = CSS::to_string(canonical_unit);
            }
            break;
        }
        case DimensionType::Flex: {
            auto flex_unit = string_to_flex_unit(unit).release_value();
            auto canonical_unit = canonical_flex_unit();
            if (flex_unit != canonical_unit && units_are_compatible(flex_unit, canonical_unit)) {
                value *= ratio_between_units(flex_unit, canonical_unit);
                unit = CSS::to_string(canonical_unit);
            }
            break;
        }
        case DimensionType::Frequency: {
            auto frequency_unit = string_to_frequency_unit(unit).release_value();
            auto canonical_unit = canonical_frequency_unit();
            if (frequency_unit != canonical_unit && units_are_compatible(frequency_unit, canonical_unit)) {
                value *= ratio_between_units(frequency_unit, canonical_unit);
                unit = CSS::to_string(canonical_unit);
            }
            break;
        }
        case DimensionType::Length: {
            auto length_unit = string_to_length_unit(unit).release_value();
            auto canonical_unit = canonical_length_unit();
            if (length_unit != canonical_unit && units_are_compatible(length_unit, canonical_unit)) {
                value *= ratio_between_units(length_unit, canonical_unit);
                unit = CSS::to_string(canonical_unit);
            }
            break;
        }
        case DimensionType::Resolution: {
            auto resolution_unit = string_to_resolution_unit(unit).release_value();
            auto canonical_unit = canonical_resolution_unit();
            if (resolution_unit != canonical_unit && units_are_compatible(resolution_unit, canonical_unit)) {
                value *= ratio_between_units(resolution_unit, canonical_unit);
                unit = CSS::to_string(canonical_unit);
            }
            break;
        }
        case DimensionType::Time: {
            auto time_unit = string_to_time_unit(unit).release_value();
            auto canonical_unit = canonical_time_unit();
            if (time_unit != canonical_unit && units_are_compatible(time_unit, canonical_unit)) {
                value *= ratio_between_units(time_unit, canonical_unit);
                unit = CSS::to_string(canonical_unit);
            }
            break;
        }
        }
    }

    // 3. If unit is "number", return «(value, «[ ]»)».
    if (unit == "number"_fly_string)
        return SumValue { SumValueItem { value, {} } };

    // 4. Otherwise, return «(value, «[unit → 1]»)».
    return SumValue { SumValueItem { value, { { unit, 1 } } } };
}

}
