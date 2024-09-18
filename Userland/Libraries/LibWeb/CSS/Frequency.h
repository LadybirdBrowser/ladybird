/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {
class Frequency {
public:
    enum class Type {
        Hz,
        kHz
    };

    static Optional<Type> unit_from_name(StringView);

    Frequency(double value, Type type);
    static Frequency make_hertz(double);
    Frequency percentage_of(Percentage const&) const;

    String to_string() const;
    double to_hertz() const;

    Type type() const { return m_type; }
    double raw_value() const { return m_value; }
    StringView unit_name() const;

    bool operator==(Frequency const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
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

    static Frequency resolve_calculated(NonnullRefPtr<CSSMathValue> const&, Layout::Node const&, Frequency const& reference_value);

private:
    Type m_type;
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
