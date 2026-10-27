/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSUnitValue.h"
#include <LibGC/Heap.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/CSS/Units.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSUnitValue);

GC::Ref<CSSUnitValue> CSSUnitValue::create(double value, Utf16FlyString unit)
{
    // The type of a CSSUnitValue is the result of creating a type from its unit internal slot.
    // https://drafts.css-houdini.org/css-typed-om-1/#type-of-a-cssunitvalue
    auto numeric_type = NumericType::create_from_unit(unit);
    return GC::Heap::the().allocate<CSSUnitValue>(value, move(unit), numeric_type.release_value());
}

GC::Ptr<CSSUnitValue> CSSUnitValue::create_from_sum_value_item(SumValueItem const& item)
{
    // 1. If item has more than one entry in its unit map, return failure.
    if (item.unit_map.size() > 1)
        return {};

    // 2. If item has no entries in its unit map, return a new CSSUnitValue whose unit internal slot is set to
    //    "number", and whose value internal slot is set to item’s value.
    if (item.unit_map.is_empty())
        return CSSUnitValue::create(item.value, "number"_utf16_fly_string);

    // 3. Otherwise, item has a single entry in its unit map. If that entry’s value is anything other than 1, return
    //    failure.
    auto single_type_entry = item.unit_map.begin();
    if (single_type_entry->value != 1)
        return {};

    // 4. Otherwise, return a new CSSUnitValue whose unit internal slot is set to that entry’s key, and whose value
    //    internal slot is set to item’s value.
    return CSSUnitValue::create(item.value, single_type_entry->key);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-cssunitvalue
WebIDL::ExceptionOr<GC::Ref<CSSUnitValue>> CSSUnitValue::construct_impl(double value, Utf16String unit)
{
    // 1. If creating a type from unit returns failure, throw a TypeError and abort this algorithm.
    auto fly_unit = Utf16FlyString { move(unit) };
    auto numeric_type = NumericType::create_from_unit(fly_unit);
    if (!numeric_type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create CSSUnitValue with unrecognized unit"_utf16 };

    // 2. Return a new CSSUnitValue with its value internal slot set to value and its unit set to unit.
    return GC::Heap::the().allocate<CSSUnitValue>(value, move(fly_unit), numeric_type.release_value());
}

CSSUnitValue::CSSUnitValue(double value, Utf16FlyString unit, NumericType type)
    : CSSNumericValue(move(type))
    , m_value(value)
    // AD-HOC: WPT expects the unit to be lowercase but this doesn't seem to be specified anywhere.
    , m_unit(unit.to_ascii_lowercase())
{
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-value
void CSSUnitValue::set_value(double value)
{
    // AD-HOC: No definition: https://github.com/w3c/css-houdini-drafts/issues/1146
    m_value = value;
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssunitvalue
void CSSUnitValue::serialize_unit_value(Utf16StringBuilder& builder, Optional<double> minimum, Optional<double> maximum) const
{
    // To serialize a CSSUnitValue this, with optional arguments minimum, a numeric value, and maximum, a numeric value:

    // 1. Let value and unit be this‘s value and unit internal slots.

    // 4. If minimum was passed and this is less than minimum, or if maximum was passed and this is greater than
    //    maximum, or either minimum and/or maximum were passed and the relative size of this and minimum/maximum can’t
    //    be determined with the available information at this time, prepend "calc(" to s, then append ")" to s.
    // NB: This step is done first to avoid prepending to the StringBuilder.
    // FIXME: "or either minimum and/or maximum were passed and the relative size of this and minimum/maximum can't be determined with the available information at this time"
    bool needs_calc_wrapper = (minimum.has_value() && m_value < minimum.value())
        || (maximum.has_value() && m_value > maximum.value());

    if (needs_calc_wrapper)
        builder.append_ascii("calc("sv);

    // 2. Set s to the result of serializing a <number> from value, per CSSOM §6.7.2 Serializing CSS Values.
    serialize_a_number(builder, m_value);

    // 3. If unit is:
    // -> "number"
    if (m_unit == "number"_utf16_fly_string) {
        // Do nothing.
    }
    // -> "percent"
    else if (m_unit == "percent"_utf16_fly_string) {
        // Append "%" to s.
        builder.append_ascii('%');
    }
    // -> anything else
    else {
        // Append unit to s.
        builder.append(m_unit.to_ascii_lowercase());
    }

    if (needs_calc_wrapper)
        builder.append_ascii(')');

    // 5. Return s.
}

// https://drafts.css-houdini.org/css-typed-om-1/#convert-a-cssunitvalue
GC::Ptr<CSSUnitValue> CSSUnitValue::converted_to_unit(Utf16FlyString const& unit) const
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
            return nullptr;

        switch (*new_dimension_type) {
        case DimensionType::Angle: {
            auto from = string_to_angle_unit(old_unit).release_value();
            auto to = string_to_angle_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return nullptr;
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Flex: {
            auto from = string_to_flex_unit(old_unit).release_value();
            auto to = string_to_flex_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return nullptr;
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Frequency: {
            auto from = string_to_frequency_unit(old_unit).release_value();
            auto to = string_to_frequency_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return nullptr;
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Length: {
            auto from = string_to_length_unit(old_unit).release_value();
            auto to = string_to_length_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return nullptr;
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Resolution: {
            auto from = string_to_resolution_unit(old_unit).release_value();
            auto to = string_to_resolution_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return nullptr;
            ratio = ratio_between_units(from, to);
            break;
        }
        case DimensionType::Time: {
            auto from = string_to_time_unit(old_unit).release_value();
            auto to = string_to_time_unit(unit).release_value();
            if (!units_are_compatible(from, to))
                return nullptr;
            ratio = ratio_between_units(from, to);
            break;
        }
        }
    }

    return CSSUnitValue::create(old_value * ratio, unit);
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
    if (unit == "number"_utf16_fly_string)
        return SumValue { SumValueItem { value, {} } };

    // 4. Otherwise, return «(value, «[unit → 1]»)».
    return SumValue { SumValueItem { value, { { unit, 1 } } } };
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> CSSUnitValue::create_an_internal_representation(PropertyNameAndID const& property, PerformTypeCheck perform_type_check) const
{
    // If value is a CSSStyleValue subclass,
    //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
    //
    //     If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of value
    //     is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it in a
    //     fresh CSSMathSum whose values internal slot contains only that part of value.
    //
    //     Return the value.

    // NB: We store all custom properties as UnresolvedStyleValue, so we always need to create one here.
    if (perform_type_check == PerformTypeCheck::Yes && property.is_custom_property()) {
        Utf16StringBuilder builder;
        serialize_unit_value(builder, {}, {});
        return UnresolvedStyleValue::create(builder.to_string(), Parser::SubstitutionFunctionsPresence {});
    }

    auto wrap_in_math_sum = [this, &property](NumericLeaf const& leaf) -> NonnullRefPtr<StyleValue const> {
        auto context = CalculationContext::for_property(property);
        Vector<CalcNodeRef> children;
        children.append(CalcNodeRef::numeric(leaf));
        auto math_sum_node = CalcNodeRef::sum(move(children));
        return CalculatedStyleValue::create(move(math_sum_node), NumericType::create_from_unit(m_unit).release_value(), context);
    };

    auto leaf = NumericLeaf::from_typed_om_unit(m_value, m_unit);
    if (!leaf.has_value()) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unrecognized unit."_utf16 };
    }

    if (perform_type_check == PerformTypeCheck::No)
        return leaf->to_style_value();

    // FIXME: Check types allowed by registered custom properties.
    auto style_value = [&] -> RefPtr<StyleValue const> {
        if (leaf->kind == NumericLeaf::Kind::Number) {
            // NB: Our parser represents an <opacity-value> as an opacity value that holds the number.
            if (property_accepts_type(property.id(), ValueType::OpacityValue))
                return OpacityValueStyleValue::create(NumberStyleValue::create(leaf->value));

            // NB: Number before Integer, because a custom property accepts either and we want to avoid rounding in that case.
            if (!property_accepts_type(property.id(), ValueType::Number) && property_accepts_type(property.id(), ValueType::Integer)) {
                auto integer = round_to_nearest_integer(leaf->value);
                // NB: A number that is not an integer does not match <integer>, so it's wrapped in a CSSMathSum, like
                //     a value outside of the accepted range. It then gets rounded when computed.
                if (static_cast<double>(integer) != leaf->value)
                    return wrap_in_math_sum(*leaf);
                if (property_accepts_value_in_range(property.id(), ValueType::Integer, static_cast<double>(integer)))
                    return IntegerStyleValue::create(integer);
                return wrap_in_math_sum(*leaf);
            }
        }

        // NB: Our parser represents an <opacity-value> as an opacity value that holds the percentage.
        if (leaf->kind == NumericLeaf::Kind::Percentage && property_accepts_type(property.id(), ValueType::OpacityValue))
            return OpacityValueStyleValue::create(leaf->to_style_value());

        if (!property_accepts_type(property.id(), leaf->value_type()))
            return {};
        if (property_accepts_value_in_range(property.id(), leaf->value_type(), leaf->value))
            return leaf->to_style_value();
        return wrap_in_math_sum(*leaf);
    }();

    if (!style_value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Property does not accept values of this type."_utf16 };
    if (!style_value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Property does not accept values of this type."_utf16 };
    return style_value.release_nonnull();
}

WebIDL::ExceptionOr<CalcNodeRef> CSSUnitValue::create_calculation_node(CalculationContext const&) const
{
    auto leaf = NumericLeaf::from_typed_om_unit(m_value, m_unit);
    if (!leaf.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unable to create calculation node."_utf16 };

    return CalcNodeRef::numeric(*leaf);
}

}
