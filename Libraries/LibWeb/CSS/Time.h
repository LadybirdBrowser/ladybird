/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class Time {
public:
    enum class Type {
        S,
        Ms,
    };

    static Optional<Type> unit_from_name(StringView);

    Time(double value, Type type);
    static Time make_seconds(double);
    Time percentage_of(Percentage const&) const;

    String to_string() const;
    double to_milliseconds() const;
    double to_seconds() const;

    Type type() const { return m_type; }
    double raw_value() const { return m_value; }
    StringView unit_name() const;

    bool operator==(Time const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
    }

    int operator<=>(Time const& other) const
    {
        auto this_seconds = to_seconds();
        auto other_seconds = other.to_seconds();

        if (this_seconds < other_seconds)
            return -1;
        if (this_seconds > other_seconds)
            return 1;
        return 0;
    }

    static Time resolve_calculated(NonnullRefPtr<CSSMathValue> const&, Layout::Node const&, Time const& reference_value);

private:
    Type m_type;
    double m_value { 0 };
};

}

template<>
struct AK::Formatter<Web::CSS::Time> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Time const& time)
    {
        return Formatter<StringView>::format(builder, time.to_string());
    }
};
