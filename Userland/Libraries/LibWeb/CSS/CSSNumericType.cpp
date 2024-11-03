/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSNumericType.h"
#include <AK/HashMap.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

Optional<CSSNumericType::BaseType> CSSNumericType::base_type_from_value_type(ValueType value_type)
{
    switch (value_type) {
    case ValueType::Angle:
        return BaseType::Angle;
    case ValueType::Flex:
        return BaseType::Flex;
    case ValueType::Frequency:
        return BaseType::Frequency;
    case ValueType::Length:
        return BaseType::Length;
    case ValueType::Percentage:
        return BaseType::Percent;
    case ValueType::Resolution:
        return BaseType::Resolution;
    case ValueType::Time:
        return BaseType::Time;

    case ValueType::BackgroundPosition:
    case ValueType::BasicShape:
    case ValueType::Color:
    case ValueType::Counter:
    case ValueType::CustomIdent:
    case ValueType::EasingFunction:
    case ValueType::FilterValueList:
    case ValueType::Image:
    case ValueType::Integer:
    case ValueType::Number:
    case ValueType::OpenTypeTag:
    case ValueType::Paint:
    case ValueType::Position:
    case ValueType::Ratio:
    case ValueType::Rect:
    case ValueType::String:
    case ValueType::Url:
        return {};
    }

    VERIFY_NOT_REACHED();
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-create-a-type
Optional<CSSNumericType> CSSNumericType::create_from_unit(StringView unit)
{
    // To create a type from a string unit, follow the appropriate branch of the following:

    // unit is "number"
    if (unit == "number"sv) {
        // Return «[ ]» (empty map)
        return CSSNumericType {};
    }

    // unit is "percent"
    if (unit == "percent"sv) {
        // Return «[ "percent" → 1 ]»
        return CSSNumericType { BaseType::Percent, 1 };
    }

    // unit is a <length> unit
    if (Length::unit_from_name(unit).has_value()) {
        // Return «[ "length" → 1 ]»
        return CSSNumericType { BaseType::Length, 1 };
    }

    // unit is an <angle> unit
    if (Angle::unit_from_name(unit).has_value()) {
        //    Return «[ "angle" → 1 ]»
        return CSSNumericType { BaseType::Angle, 1 };
    }

    // unit is a <time> unit
    if (Time::unit_from_name(unit).has_value()) {
        //    Return «[ "time" → 1 ]»
        return CSSNumericType { BaseType::Time, 1 };
    }

    // unit is a <frequency> unit
    if (Frequency::unit_from_name(unit).has_value()) {
        //    Return «[ "frequency" → 1 ]»
        return CSSNumericType { BaseType::Frequency, 1 };
    }

    // unit is a <resolution> unit
    if (Resolution::unit_from_name(unit).has_value()) {
        //    Return «[ "resolution" → 1 ]»
        return CSSNumericType { BaseType::Resolution, 1 };
    }

    // unit is a <flex> unit
    // FIXME: We don't have <flex> as a type yet.
    //    Return «[ "flex" → 1 ]»

    // anything else
    //    Return failure.
    return {};

    // In all cases, the associated percent hint is null.
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-add-two-types
Optional<CSSNumericType> CSSNumericType::added_to(CSSNumericType const& other) const
{
    // To add two types type1 and type2, perform the following steps:

    // 1. Replace type1 with a fresh copy of type1, and type2 with a fresh copy of type2.
    //    Let finalType be a new type with an initially empty ordered map and an initially null percent hint.
    CSSNumericType type1 = *this;
    CSSNumericType type2 = other;
    CSSNumericType final_type {};

    // 2. If both type1 and type2 have non-null percent hints with different values
    if (type1.percent_hint().has_value() && type2.percent_hint().has_value() && type1.percent_hint() != type2.percent_hint()) {
        // The types can’t be added. Return failure.
        return {};
    }
    //    If type1 has a non-null percent hint hint and type2 doesn’t
    else if (type1.percent_hint().has_value() && !type2.percent_hint().has_value()) {
        // Apply the percent hint hint to type2.
        type2.apply_percent_hint(type1.percent_hint().value());
    }
    //    Vice versa if type2 has a non-null percent hint and type1 doesn’t.
    else if (type2.percent_hint().has_value() && !type1.percent_hint().has_value()) {
        type1.apply_percent_hint(type2.percent_hint().value());
    }
    // Otherwise
    //     Continue to the next step.

    // 3. If all the entries of type1 with non-zero values are contained in type2 with the same value, and vice-versa
    if (type2.contains_all_the_non_zero_entries_of_other_with_the_same_value(type1)
        && type1.contains_all_the_non_zero_entries_of_other_with_the_same_value(type2)) {
        // Copy all of type1’s entries to finalType, and then copy all of type2’s entries to finalType that
        // finalType doesn’t already contain. Set finalType’s percent hint to type1’s percent hint. Return finalType.
        final_type.copy_all_entries_from(type1, SkipIfAlreadyPresent::No);
        final_type.copy_all_entries_from(type2, SkipIfAlreadyPresent::Yes);
        final_type.set_percent_hint(type1.percent_hint());
        return final_type;
    }
    //    If type1 and/or type2 contain "percent" with a non-zero value,
    //    and type1 and/or type2 contain a key other than "percent" with a non-zero value
    else if ((type1.exponent(BaseType::Percent) != 0 || type2.exponent(BaseType::Percent) != 0)
        && (type1.contains_a_key_other_than_percent_with_a_non_zero_value() || type2.contains_a_key_other_than_percent_with_a_non_zero_value())) {
        // For each base type other than "percent" hint:
        for (auto hint_int = 0; hint_int < to_underlying(BaseType::__Count); ++hint_int) {
            auto hint = static_cast<BaseType>(hint_int);
            if (hint == BaseType::Percent)
                continue;

            // 1. Provisionally apply the percent hint hint to both type1 and type2.
            auto provisional_type1 = type1;
            provisional_type1.apply_percent_hint(hint);
            auto provisional_type2 = type2;
            provisional_type2.apply_percent_hint(hint);

            // 2. If, afterwards, all the entries of type1 with non-zero values are contained in type2
            //    with the same value, and vice versa, then copy all of type1’s entries to finalType,
            //    and then copy all of type2’s entries to finalType that finalType doesn’t already contain.
            //    Set finalType’s percent hint to hint. Return finalType.
            if (provisional_type2.contains_all_the_non_zero_entries_of_other_with_the_same_value(provisional_type1)
                && provisional_type1.contains_all_the_non_zero_entries_of_other_with_the_same_value(provisional_type2)) {

                final_type.copy_all_entries_from(provisional_type1, SkipIfAlreadyPresent::No);
                final_type.copy_all_entries_from(provisional_type2, SkipIfAlreadyPresent::Yes);
                final_type.set_percent_hint(hint);
                return final_type;
            }

            // 3. Otherwise, revert type1 and type2 to their state at the start of this loop.
            // NOTE: We did the modifications to provisional_type1/2 so this is a no-op.
        }

        // If the loop finishes without returning finalType, then the types can’t be added. Return failure.
        return {};
    }
    // Otherwise
    //     The types can’t be added. Return failure.
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-multiply-two-types
Optional<CSSNumericType> CSSNumericType::multiplied_by(CSSNumericType const& other) const
{
    // To multiply two types type1 and type2, perform the following steps:

    // 1. Replace type1 with a fresh copy of type1, and type2 with a fresh copy of type2.
    //    Let finalType be a new type with an initially empty ordered map and an initially null percent hint.
    CSSNumericType type1 = *this;
    CSSNumericType type2 = other;
    CSSNumericType final_type {};

    // 2. If both type1 and type2 have non-null percent hints with different values,
    //    the types can’t be multiplied. Return failure.
    if (type1.percent_hint().has_value() && type2.percent_hint().has_value() && type1.percent_hint() != type2.percent_hint())
        return {};

    // 3. If type1 has a non-null percent hint hint and type2 doesn’t, apply the percent hint hint to type2.
    if (type1.percent_hint().has_value() && !type2.percent_hint().has_value()) {
        type2.apply_percent_hint(type1.percent_hint().value());
    }
    //    Vice versa if type2 has a non-null percent hint and type1 doesn’t.
    else if (type2.percent_hint().has_value() && !type1.percent_hint().has_value()) {
        type1.apply_percent_hint(type2.percent_hint().value());
    }

    // 4. Copy all of type1’s entries to finalType, then for each baseType → power of type2:
    final_type.copy_all_entries_from(type1, SkipIfAlreadyPresent::No);
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        if (!type2.exponent(base_type).has_value())
            continue;
        auto power = type2.exponent(base_type).value();

        // 1. If finalType[baseType] exists, increment its value by power.
        if (auto exponent = final_type.exponent(base_type); exponent.has_value()) {
            final_type.set_exponent(base_type, exponent.value() + power);
        }
        // 2. Otherwise, set finalType[baseType] to power.
        else {
            final_type.set_exponent(base_type, power);
        }
    }
    //    Set finalType’s percent hint to type1’s percent hint.
    final_type.set_percent_hint(type1.percent_hint());

    // 5. Return finalType.
    return final_type;
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-invert-a-type
CSSNumericType CSSNumericType::inverted() const
{
    // To invert a type type, perform the following steps:

    // 1. Let result be a new type with an initially empty ordered map and an initially null percent hint
    CSSNumericType result;

    // 2. For each unit → exponent of type, set result[unit] to (-1 * exponent).
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        if (!exponent(base_type).has_value())
            continue;
        auto power = exponent(base_type).value();
        result.set_exponent(base_type, -1 * power);
    }

    // 3. Return result.
    return result;
}

// https://drafts.css-houdini.org/css-typed-om-1/#apply-the-percent-hint
void CSSNumericType::apply_percent_hint(BaseType hint)
{
    // To apply the percent hint hint to a type, perform the following steps:

    // 1. If type doesn’t contain hint, set type[hint] to 0.
    if (!exponent(hint).has_value())
        set_exponent(hint, 0);

    // 2. If type contains "percent", add type["percent"] to type[hint], then set type["percent"] to 0.
    if (exponent(BaseType::Percent).has_value()) {
        set_exponent(hint, exponent(BaseType::Percent).value() + exponent(hint).value());
        set_exponent(BaseType::Percent, 0);
    }

    // 3. Set type’s percent hint to hint.
    set_percent_hint(hint);
}

bool CSSNumericType::contains_all_the_non_zero_entries_of_other_with_the_same_value(CSSNumericType const& other) const
{
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto other_exponent = other.exponent(static_cast<BaseType>(i));
        if (other_exponent.has_value() && other_exponent != 0
            && this->exponent(static_cast<BaseType>(i)) != other_exponent) {
            return false;
        }
    }
    return true;
}

bool CSSNumericType::contains_a_key_other_than_percent_with_a_non_zero_value() const
{
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        if (i == to_underlying(BaseType::Percent))
            continue;
        if (m_type_exponents[i].has_value() && m_type_exponents[i] != 0)
            return true;
    }
    return false;
}

void CSSNumericType::copy_all_entries_from(CSSNumericType const& other, SkipIfAlreadyPresent ignore_existing_values)
{
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto exponent = other.exponent(base_type);
        if (!exponent.has_value())
            continue;
        if (ignore_existing_values == SkipIfAlreadyPresent::Yes && this->exponent(base_type).has_value())
            continue;
        set_exponent(base_type, *exponent);
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-match
bool CSSNumericType::matches_dimension(BaseType type) const
{
    // A type matches <length> if its only non-zero entry is «[ "length" → 1 ]» and its percent hint is null.
    // Similarly for <angle>, <time>, <frequency>, <resolution>, and <flex>.

    if (percent_hint().has_value())
        return false;

    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto type_exponent = exponent(base_type);
        if (base_type == type) {
            if (type_exponent != 1)
                return false;
        } else {
            if (type_exponent.has_value() && type_exponent != 0)
                return false;
        }
    }

    return true;
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-match
bool CSSNumericType::matches_percentage() const
{
    // A type matches <percentage> if its only non-zero entry is «[ "percent" → 1 ]».
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto type_exponent = exponent(base_type);
        if (base_type == BaseType::Percent) {
            if (type_exponent != 1)
                return false;
        } else {
            if (type_exponent.has_value() && type_exponent != 0)
                return false;
        }
    }
    return true;
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-match
bool CSSNumericType::matches_dimension_percentage(BaseType type) const
{
    // A type matches <length-percentage> if its only non-zero entry is either «[ "length" → 1 ]»
    // or «[ "percent" → 1 ]» Same for <angle-percentage>, <time-percentage>, etc.

    // Check for percent -> 1 or type -> 1, but not both
    if ((exponent(type) == 1) == (exponent(BaseType::Percent) == 1))
        return false;

    // Ensure all other types are absent or 0
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto type_exponent = exponent(base_type);
        if (base_type == type || base_type == BaseType::Percent)
            continue;

        if (type_exponent.has_value() && type_exponent != 0)
            return false;
    }

    return true;
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-match
bool CSSNumericType::matches_number() const
{
    // A type matches <number> if it has no non-zero entries and its percent hint is null.
    if (percent_hint().has_value())
        return false;

    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto type_exponent = exponent(base_type);
        if (type_exponent.has_value() && type_exponent != 0)
            return false;
    }

    return true;
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-match
bool CSSNumericType::matches_number_percentage() const
{
    // A type matches <number-percentage> if it has no non-zero entries, or its only non-zero entry is «[ "percent" → 1 ]».
    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto type_exponent = exponent(base_type);

        if (base_type == BaseType::Percent) {
            if (type_exponent.has_value() && type_exponent != 0 && type_exponent != 1)
                return false;
        } else if (type_exponent.has_value() && type_exponent != 0) {
            return false;
        }
    }

    return true;
}

bool CSSNumericType::matches_dimension() const
{
    // This isn't a spec algorithm.
    // A type should match `<dimension>` if there are no non-zero entries,
    // or it has a single non-zero entry (other than percent) which is equal to 1.

    auto number_of_one_exponents = 0;

    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto type_exponent = exponent(base_type);
        if (!type_exponent.has_value())
            continue;

        if (type_exponent == 1) {
            if (base_type == BaseType::Percent)
                return false;
            number_of_one_exponents++;
        } else if (type_exponent != 0) {
            return false;
        }
    }

    return number_of_one_exponents == 0 || number_of_one_exponents == 1;
}

ErrorOr<String> CSSNumericType::dump() const
{
    StringBuilder builder;
    TRY(builder.try_appendff("{{ hint: {}", m_percent_hint.map([](auto base_type) { return base_type_name(base_type); })));

    for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
        auto base_type = static_cast<BaseType>(i);
        auto type_exponent = exponent(base_type);

        if (type_exponent.has_value())
            TRY(builder.try_appendff(", \"{}\" → {}", base_type_name(base_type), type_exponent.value()));
    }

    TRY(builder.try_append(" }"sv));
    return builder.to_string();
}

}
