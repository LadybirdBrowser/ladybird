/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class AngleStyleValue : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<AngleStyleValue> create(Angle angle)
    {
        return adopt_ref(*new (nothrow) AngleStyleValue(move(angle)));
    }
    virtual ~AngleStyleValue() override;

    Angle const& angle() const { return m_angle; }
    virtual double value() const override { return m_angle.raw_value(); }
    virtual StringView unit() const override { return m_angle.unit_name(); }

    virtual String to_string() const override;

    bool equals(CSSStyleValue const& other) const override;

private:
    explicit AngleStyleValue(Angle angle);

    Angle m_angle;
};

}
