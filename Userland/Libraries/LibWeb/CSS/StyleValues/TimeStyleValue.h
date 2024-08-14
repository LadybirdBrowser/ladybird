/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class TimeStyleValue : public StyleValueWithDefaultOperators<TimeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<TimeStyleValue> create(Time time)
    {
        return adopt_ref(*new (nothrow) TimeStyleValue(move(time)));
    }
    virtual ~TimeStyleValue() override = default;

    Time const& time() const { return m_time; }

    virtual String to_string() const override { return m_time.to_string(); }

    bool properties_equal(TimeStyleValue const& other) const { return m_time == other.m_time; }

private:
    explicit TimeStyleValue(Time time)
        : StyleValueWithDefaultOperators(Type::Time)
        , m_time(move(time))
    {
    }

    Time m_time;
};

}
