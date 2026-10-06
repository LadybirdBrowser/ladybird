/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
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

// https://drafts.csswg.org/css-grid-2/#typedef-flex
class Flex {
public:
    Flex(double value, FlexUnit unit);

    double to_fr() const;

    double raw_value() const { return m_value; }
    FlexUnit unit() const { return m_unit; }
    Utf16FlyString unit_name() const { return CSS::to_string(m_unit); }

    bool operator==(Flex const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    int operator<=>(Flex const& other) const
    {
        auto this_fr = to_fr();
        auto other_fr = other.to_fr();

        if (this_fr < other_fr)
            return -1;
        if (this_fr > other_fr)
            return 1;
        return 0;
    }

private:
    FlexUnit m_unit;
    double m_value { 0 };
};

}
