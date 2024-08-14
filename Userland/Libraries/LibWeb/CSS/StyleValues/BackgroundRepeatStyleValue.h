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
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

class BackgroundRepeatStyleValue final : public StyleValueWithDefaultOperators<BackgroundRepeatStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BackgroundRepeatStyleValue> create(Repeat repeat_x, Repeat repeat_y)
    {
        return adopt_ref(*new (nothrow) BackgroundRepeatStyleValue(repeat_x, repeat_y));
    }
    virtual ~BackgroundRepeatStyleValue() override;

    Repeat repeat_x() const { return m_properties.repeat_x; }
    Repeat repeat_y() const { return m_properties.repeat_y; }

    virtual String to_string() const override;

    bool properties_equal(BackgroundRepeatStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    BackgroundRepeatStyleValue(Repeat repeat_x, Repeat repeat_y);

    struct Properties {
        Repeat repeat_x;
        Repeat repeat_y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
