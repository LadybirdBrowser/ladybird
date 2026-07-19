/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>

namespace Web::CSS {

class AngleStyleValue : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<AngleStyleValue const> create(Angle angle)
    {
        return adopt_ref(*new (nothrow) AngleStyleValue(move(angle)));
    }
    virtual ~AngleStyleValue() override;

    Angle angle() const { return Angle(m_value->angle.value, static_cast<AngleUnit>(m_value->angle.unit)); }
    virtual double raw_value() const override { return m_value->angle.value; }
    virtual Utf16FlyString unit_name() const override { return angle().unit_name(); }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    void serialize(StringBuilder&, SerializationMode) const;

    bool equals(StyleValue const& other) const;

private:
    explicit AngleStyleValue(Angle angle);
};

}
