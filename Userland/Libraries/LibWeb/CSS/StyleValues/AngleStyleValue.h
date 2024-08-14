/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class AngleStyleValue : public StyleValueWithDefaultOperators<AngleStyleValue> {
public:
    static ValueComparingNonnullRefPtr<AngleStyleValue> create(Angle angle)
    {
        return adopt_ref(*new (nothrow) AngleStyleValue(move(angle)));
    }
    virtual ~AngleStyleValue() override;

    Angle const& angle() const { return m_angle; }

    virtual String to_string() const override;

    bool properties_equal(AngleStyleValue const& other) const;

private:
    explicit AngleStyleValue(Angle angle);

    Angle m_angle;
};

}
