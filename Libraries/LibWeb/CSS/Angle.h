/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/Units.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class Angle {
public:
    Angle(double value, AngleUnit unit);
    static Angle make_degrees(double);
    Angle percentage_of(Percentage const&) const;

    String to_string(SerializationMode = SerializationMode::Normal) const;

    double to_degrees() const;
    double to_radians() const;

    double raw_value() const { return m_value; }
    AngleUnit unit() const { return m_unit; }
    FlyString unit_name() const { return CSS::to_string(m_unit); }

    bool operator==(Angle const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    int operator<=>(Angle const& other) const
    {
        auto this_degrees = to_degrees();
        auto other_degrees = other.to_degrees();

        if (this_degrees < other_degrees)
            return -1;
        if (this_degrees > other_degrees)
            return 1;
        return 0;
    }

    static Angle from_style_value(NonnullRefPtr<StyleValue const> const&, Optional<Angle> percentage_basis);
    static Angle resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const&, Layout::Node const&, Angle const& reference_value);

private:
    AngleUnit m_unit;
    double m_value { 0 };
};

}

template<>
struct AK::Formatter<Web::CSS::Angle> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Angle const& angle)
    {
        return Formatter<StringView>::format(builder, angle.to_string());
    }
};
