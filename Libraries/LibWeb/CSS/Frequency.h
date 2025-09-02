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

class Frequency {
public:
    Frequency(double value, FrequencyUnit unit);
    static Frequency make_hertz(double);
    Frequency percentage_of(Percentage const&) const;

    String to_string(SerializationMode = SerializationMode::Normal) const;
    double to_hertz() const;

    double raw_value() const { return m_value; }
    FrequencyUnit unit() const { return m_unit; }
    StringView unit_name() const { return CSS::to_string(m_unit); }

    bool operator==(Frequency const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    int operator<=>(Frequency const& other) const
    {
        auto this_hertz = to_hertz();
        auto other_hertz = other.to_hertz();

        if (this_hertz < other_hertz)
            return -1;
        if (this_hertz > other_hertz)
            return 1;
        return 0;
    }

    static Frequency resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const&, Layout::Node const&, Frequency const& reference_value);

private:
    FrequencyUnit m_unit;
    double m_value { 0 };
};

}

template<>
struct AK::Formatter<Web::CSS::Frequency> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Frequency const& frequency)
    {
        return Formatter<StringView>::format(builder, frequency.to_string());
    }
};
