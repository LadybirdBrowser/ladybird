/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class Percentage {
public:
    explicit Percentage(double value)
        : m_value(value)
    {
    }

    double value() const { return m_value; }
    double as_fraction() const { return m_value * 0.01; }

    String to_string() const
    {
        return MUST(String::formatted("{}%", m_value));
    }

    bool operator==(Percentage const& other) const { return m_value == other.m_value; }

    int operator<=>(Percentage const& other) const
    {
        if (m_value < other.m_value)
            return -1;
        if (m_value > other.m_value)
            return 1;
        return 0;
    }

private:
    double m_value;
};

}
