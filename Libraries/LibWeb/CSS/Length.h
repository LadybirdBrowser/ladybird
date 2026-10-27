/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/Units.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::CSS {

class WEB_API Length {
public:
    Length(double value, LengthUnit unit)
        : m_unit(unit)
        , m_value(value)
    {
    }
    ~Length() = default;

    [[nodiscard]] static Length make_px(double value) { return Length(value, LengthUnit::Px); }
    [[nodiscard]] static Length make_px(CSSPixels value) { return make_px(value.to_double()); }
    Length percentage_of(Percentage const&) const;

    bool is_px() const { return m_unit == LengthUnit::Px; }

    bool is_absolute() const { return CSS::is_absolute(m_unit); }

    double raw_value() const { return m_value; }
    LengthUnit unit() const { return m_unit; }
    Utf16FlyString unit_name() const { return CSS::to_string(m_unit); }

    [[nodiscard]] ALWAYS_INLINE CSSPixels absolute_length_to_px() const
    {
        return CSSPixels::nearest_value_for(absolute_length_to_px_without_rounding());
    }

    [[nodiscard]] ALWAYS_INLINE double absolute_length_to_px_without_rounding() const
    {
        return ratio_between_units(m_unit, LengthUnit::Px) * m_value;
    }

    void serialize(StringBuilder&, SerializationMode = SerializationMode::Normal) const;
    void serialize(Utf16StringBuilder&, SerializationMode = SerializationMode::Normal) const;

    bool operator==(Length const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    static Length from_style_value(NonnullRefPtr<StyleValue const> const&, Optional<Length> percentage_basis);

private:
    LengthUnit m_unit;
    double m_value { 0 };
};

}
