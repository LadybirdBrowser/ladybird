/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class Angle {
public:
    enum class Type {
        Deg,
        Grad,
        Rad,
        Turn,
    };

    static Optional<Type> unit_from_name(StringView);

    Angle(double value, Type type);
    static Angle make_degrees(double);
    Angle percentage_of(Percentage const&) const;

    String to_string() const;

    double to_degrees() const;
    double to_radians() const;

    Type type() const { return m_type; }
    double raw_value() const { return m_value; }
    StringView unit_name() const;

    bool operator==(Angle const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
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

    static Angle resolve_calculated(NonnullRefPtr<CSSMathValue> const&, Layout::Node const&, Angle const& reference_value);

private:
    Type m_type;
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
