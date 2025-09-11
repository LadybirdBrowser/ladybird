/*
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

using UnitMap = HashMap<FlyString, i32>;

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-type
class NumericType {
public:
    enum class BaseType {
        Length,
        Angle,
        Time,
        Frequency,
        Resolution,
        Flex,
        Percent,
        __Count,
    };

    static Optional<BaseType> base_type_from_value_type(ValueType);
    static constexpr StringView base_type_name(BaseType base_type)
    {
        switch (base_type) {
        case BaseType::Length:
            return "length"sv;
        case BaseType::Angle:
            return "angle"sv;
        case BaseType::Time:
            return "time"sv;
        case BaseType::Frequency:
            return "frequency"sv;
        case BaseType::Resolution:
            return "resolution"sv;
        case BaseType::Flex:
            return "flex"sv;
        case BaseType::Percent:
            return "percent"sv;
        case BaseType::__Count:
            break;
        }
        VERIFY_NOT_REACHED();
    }

    static Optional<NumericType> create_from_unit(FlyString const& unit);
    static Optional<NumericType> create_from_unit_map(UnitMap const&);
    NumericType() = default;
    NumericType(BaseType type, i32 power)
    {
        set_exponent(type, power);
    }

    Optional<NumericType> added_to(NumericType const& other) const;
    Optional<NumericType> multiplied_by(NumericType const& other) const;
    NumericType inverted() const;

    bool has_consistent_type_with(NumericType const& other) const;
    Optional<NumericType> consistent_type(NumericType const& other) const;
    Optional<NumericType> made_consistent_with(NumericType const& other) const;

    bool matches_angle(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Angle, percentages_resolve_as); }
    bool matches_angle_percentage(Optional<ValueType> percentages_resolve_as) const { return matches_dimension_percentage(BaseType::Angle, percentages_resolve_as); }
    bool matches_flex(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Flex, percentages_resolve_as); }
    bool matches_frequency(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Frequency, percentages_resolve_as); }
    bool matches_frequency_percentage(Optional<ValueType> percentages_resolve_as) const { return matches_dimension_percentage(BaseType::Frequency, percentages_resolve_as); }
    bool matches_length(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Length, percentages_resolve_as); }
    bool matches_length_percentage(Optional<ValueType> percentages_resolve_as) const { return matches_dimension_percentage(BaseType::Length, percentages_resolve_as); }
    bool matches_number(Optional<ValueType> percentages_resolve_as) const;
    bool matches_percentage() const;
    bool matches_resolution(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Resolution, percentages_resolve_as); }
    bool matches_time(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Time, percentages_resolve_as); }
    bool matches_time_percentage(Optional<ValueType> percentages_resolve_as) const { return matches_dimension_percentage(BaseType::Time, percentages_resolve_as); }

    bool matches_dimension() const;

    Optional<i32> const& exponent(BaseType type) const { return m_type_exponents[to_underlying(type)]; }
    void set_exponent(BaseType type, i32 exponent) { m_type_exponents[to_underlying(type)] = exponent; }

    template<typename Callback>
    void for_each_type_and_exponent(Callback callback) const
    {
        for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
            if (!m_type_exponents[i].has_value())
                continue;
            auto base_type = static_cast<BaseType>(i);
            callback(base_type, m_type_exponents[i].value());
        }
    }

    Optional<BaseType> const& percent_hint() const { return m_percent_hint; }
    void set_percent_hint(Optional<BaseType> hint) { m_percent_hint = hint; }
    void apply_percent_hint(BaseType hint);

    bool operator==(NumericType const& other) const = default;

    String dump() const;

private:
    bool contains_all_the_non_zero_entries_of_other_with_the_same_value(NumericType const& other) const;
    bool contains_a_key_other_than_percent_with_a_non_zero_value() const;
    enum class SkipIfAlreadyPresent {
        No,
        Yes,
    };
    void copy_all_entries_from(NumericType const& other, SkipIfAlreadyPresent);

    Optional<BaseType> entry_with_value_1_while_all_others_are_0() const;
    bool matches_dimension(BaseType, Optional<ValueType> percentages_resolve_as) const;
    bool matches_dimension_percentage(BaseType, Optional<ValueType> percentages_resolve_as) const;

    Array<Optional<i32>, to_underlying(BaseType::__Count)> m_type_exponents;
    Optional<BaseType> m_percent_hint;
};

}

template<>
struct AK::Formatter<Web::CSS::NumericType> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::NumericType const& value)
    {
        return Formatter<StringView>::format(builder, value.dump());
    }
};
