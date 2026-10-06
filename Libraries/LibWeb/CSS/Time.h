/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/Units.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class Time {
public:
    Time(double value, TimeUnit unit);
    static Time make_seconds(double);

    double to_milliseconds() const;
    double to_seconds() const;

    double raw_value() const { return m_value; }
    TimeUnit unit() const { return m_unit; }
    Utf16FlyString unit_name() const { return CSS::to_string(m_unit); }

    bool operator==(Time const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
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

private:
    TimeUnit m_unit;
    double m_value { 0 };
};

}
