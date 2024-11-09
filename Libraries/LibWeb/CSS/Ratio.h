/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-values-4/#ratios
class Ratio {
public:
    Ratio(double first, double second = 1);
    double numerator() const { return m_first_value; }
    double denominator() const { return m_second_value; }
    double value() const { return m_first_value / m_second_value; }
    bool is_degenerate() const;

    String to_string() const;

    bool operator==(Ratio const& other) const
    {
        return value() == other.value();
    }

    int operator<=>(Ratio const& other) const
    {
        auto this_value = value();
        auto other_value = other.value();

        if (this_value < other_value)
            return -1;
        if (this_value > other_value)
            return 1;
        return 0;
    }

private:
    double m_first_value { 0 };
    double m_second_value { 1 };
};

}
