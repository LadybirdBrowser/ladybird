/*
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/Forward.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

using UnitMap = HashMap<Utf16FlyString, i32>;
UnitMap product_of_two_unit_maps(UnitMap const&, UnitMap const&);

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-type
class WEB_API NumericType {
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
    static Optional<NumericType> create_from_unit(Utf16FlyString const& unit);
    static Optional<NumericType> create_from_unit_map(UnitMap const&);
    static Optional<NumericType> from_ffi(StyleValueFFI::FfiNumericType const& type)
    {
        if (!type.valid)
            return {};
        return NumericType { type };
    }
    NumericType() = default;
    NumericType(BaseType type, i32 power)
    {
        set_exponent(type, power);
    }

    StyleValueFFI::FfiNumericType const& to_ffi() const { return m_type; }

    Optional<NumericType> added_to(NumericType const& other) const;
    Optional<NumericType> multiplied_by(NumericType const& other) const;
    NumericType inverted() const;

    bool matches_angle(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Angle, percentages_resolve_as); }
    bool matches_flex(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Flex, percentages_resolve_as); }
    bool matches_frequency(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Frequency, percentages_resolve_as); }
    bool matches_length(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Length, percentages_resolve_as); }
    bool matches_length_percentage(Optional<ValueType> percentages_resolve_as) const { return matches_dimension_percentage(BaseType::Length, percentages_resolve_as); }
    bool matches_number(Optional<ValueType> percentages_resolve_as) const;
    bool matches_percentage() const;
    bool matches_resolution(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Resolution, percentages_resolve_as); }
    bool matches_time(Optional<ValueType> percentages_resolve_as) const { return matches_dimension(BaseType::Time, percentages_resolve_as); }

    Optional<i32> exponent(BaseType type) const
    {
        if (!has_exponent(to_underlying(type)))
            return {};
        return m_type.exponents[to_underlying(type)];
    }
    void set_exponent(BaseType type, i32 exponent)
    {
        m_type.has_exponent_bits |= 1u << to_underlying(type);
        m_type.exponents[to_underlying(type)] = exponent;
    }

    template<typename Callback>
    void for_each_type_and_exponent(Callback callback) const
    {
        for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
            if (!has_exponent(i))
                continue;
            callback(static_cast<BaseType>(i), m_type.exponents[i]);
        }
    }

    Optional<BaseType> percent_hint() const
    {
        if (!m_type.has_percent_hint)
            return {};
        return static_cast<BaseType>(m_type.percent_hint);
    }
    void set_percent_hint(Optional<BaseType> hint)
    {
        m_type.has_percent_hint = hint.has_value();
        m_type.percent_hint = hint.has_value() ? to_underlying(*hint) : 0;
    }

    bool operator==(NumericType const& other) const
    {
        if (m_type.has_exponent_bits != other.m_type.has_exponent_bits || percent_hint() != other.percent_hint())
            return false;
        for (auto i = 0; i < to_underlying(BaseType::__Count); ++i) {
            if (has_exponent(i) && m_type.exponents[i] != other.m_type.exponents[i])
                return false;
        }
        return true;
    }

private:
    explicit NumericType(StyleValueFFI::FfiNumericType const& type)
        : m_type(type)
    {
    }

    bool has_exponent(int base_type_index) const { return m_type.has_exponent_bits & (1u << base_type_index); }

    bool matches_dimension(BaseType, Optional<ValueType> percentages_resolve_as) const;
    bool matches_dimension_percentage(BaseType, Optional<ValueType> percentages_resolve_as) const;

    StyleValueFFI::FfiNumericType m_type { .exponents = {}, .has_exponent_bits = 0, .has_percent_hint = false, .percent_hint = 0, .valid = true };
    static_assert(sizeof(StyleValueFFI::FfiNumericType::exponents) == sizeof(i32) * to_underlying(BaseType::__Count));
};

}
