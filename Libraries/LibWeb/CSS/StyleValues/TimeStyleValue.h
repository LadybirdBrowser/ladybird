/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class TimeStyleValue : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<TimeStyleValue const> create(Time time)
    {
        return adopt_ref(*new (nothrow) TimeStyleValue(move(time)));
    }
    virtual ~TimeStyleValue() override = default;

    Time time() const { return Time(m_value->time.value, static_cast<TimeUnit>(m_value->time.unit)); }
    virtual double raw_value() const override { return m_value->time.value; }
    virtual Utf16FlyString unit_name() const override { return time().unit_name(); }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    void serialize(StringBuilder& builder, SerializationMode mode) const { time().serialize(builder, mode); }

    bool equals(StyleValue const& other) const;

private:
    explicit TimeStyleValue(Time time)
        : DimensionStyleValue(Type::Time, StyleValueFFI::rust_style_value_create_time(time.raw_value(), to_underlying(time.unit())))
    {
    }
};

}
