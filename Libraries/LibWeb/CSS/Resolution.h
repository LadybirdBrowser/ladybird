/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/Units.h>

namespace Web::CSS {

class Resolution {
public:
    Resolution(double value, ResolutionUnit unit);
    static Resolution make_dots_per_pixel(double);

    String to_string(SerializationMode = SerializationMode::Normal) const;
    double to_dots_per_pixel() const;

    double raw_value() const { return m_value; }
    ResolutionUnit unit() const { return m_unit; }
    FlyString unit_name() const { return CSS::to_string(m_unit); }

    bool operator==(Resolution const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    int operator<=>(Resolution const& other) const
    {
        auto this_dots_per_pixel = to_dots_per_pixel();
        auto other_dots_per_pixel = other.to_dots_per_pixel();

        if (this_dots_per_pixel < other_dots_per_pixel)
            return -1;
        if (this_dots_per_pixel > other_dots_per_pixel)
            return 1;
        return 0;
    }

private:
    ResolutionUnit m_unit;
    double m_value { 0 };
};

}
