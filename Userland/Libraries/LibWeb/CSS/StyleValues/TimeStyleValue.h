/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class TimeStyleValue : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<TimeStyleValue> create(Time time)
    {
        return adopt_ref(*new (nothrow) TimeStyleValue(move(time)));
    }
    virtual ~TimeStyleValue() override = default;

    Time const& time() const { return m_time; }
    virtual double value() const override { return m_time.raw_value(); }
    virtual StringView unit() const override { return m_time.unit_name(); }

    virtual String to_string() const override { return m_time.to_string(); }

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_time = other.as_time();
        return m_time == other_time.m_time;
    }

private:
    explicit TimeStyleValue(Time time)
        : CSSUnitValue(Type::Time)
        , m_time(move(time))
    {
    }

    Time m_time;
};

}
