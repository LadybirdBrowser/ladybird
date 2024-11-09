/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Flex.h>
#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS::Parser {

class Dimension {
public:
    Dimension(Angle&& value)
        : m_value(move(value))
    {
    }

    Dimension(Flex&& value)
        : m_value(move(value))
    {
    }

    Dimension(Frequency&& value)
        : m_value(move(value))
    {
    }

    Dimension(Length&& value)
        : m_value(move(value))
    {
    }
    Dimension(Percentage&& value)
        : m_value(move(value))
    {
    }

    Dimension(Resolution&& value)
        : m_value(move(value))
    {
    }

    Dimension(Time&& value)
        : m_value(move(value))
    {
    }

    bool is_angle() const { return m_value.has<Angle>(); }
    Angle angle() const { return m_value.get<Angle>(); }

    bool is_angle_percentage() const { return is_angle() || is_percentage(); }
    AnglePercentage angle_percentage() const
    {
        if (is_angle())
            return angle();
        return percentage();
    }

    bool is_flex() const { return m_value.has<Flex>(); }
    Flex flex() const { return m_value.get<Flex>(); }

    bool is_frequency() const { return m_value.has<Frequency>(); }
    Frequency frequency() const { return m_value.get<Frequency>(); }

    bool is_frequency_percentage() const { return is_frequency() || is_percentage(); }
    FrequencyPercentage frequency_percentage() const
    {
        if (is_frequency())
            return frequency();
        return percentage();
    }

    bool is_length() const { return m_value.has<Length>(); }
    Length length() const { return m_value.get<Length>(); }

    bool is_length_percentage() const { return is_length() || is_percentage(); }
    LengthPercentage length_percentage() const
    {
        if (is_length())
            return length();
        return percentage();
    }

    bool is_percentage() const { return m_value.has<Percentage>(); }
    Percentage percentage() const { return m_value.get<Percentage>(); }

    bool is_resolution() const { return m_value.has<Resolution>(); }
    Resolution resolution() const { return m_value.get<Resolution>(); }

    bool is_time() const { return m_value.has<Time>(); }
    Time time() const { return m_value.get<Time>(); }

    bool is_time_percentage() const { return is_time() || is_percentage(); }
    TimePercentage time_percentage() const
    {
        if (is_time())
            return time();
        return percentage();
    }

private:
    Variant<Angle, Flex, Frequency, Length, Percentage, Resolution, Time> m_value;
};

}
