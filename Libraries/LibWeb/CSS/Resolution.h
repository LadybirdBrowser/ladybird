/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class Resolution {
public:
    enum class Type {
        Dpi,
        Dpcm,
        Dppx,
    };

    static Optional<Type> unit_from_name(StringView);

    Resolution(double value, Type type);
    static Resolution make_dots_per_pixel(double);

    String to_string() const;
    double to_dots_per_pixel() const;

    Type type() const { return m_type; }
    double raw_value() const { return m_value; }
    StringView unit_name() const;

    bool operator==(Resolution const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
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
    Type m_type;
    double m_value { 0 };
};
}
