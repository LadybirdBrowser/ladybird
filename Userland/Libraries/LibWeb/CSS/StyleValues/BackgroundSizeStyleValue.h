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
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

// NOTE: This is not used for identifier sizes, like `cover` and `contain`.
class BackgroundSizeStyleValue final : public StyleValueWithDefaultOperators<BackgroundSizeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BackgroundSizeStyleValue> create(LengthPercentage size_x, LengthPercentage size_y)
    {
        return adopt_ref(*new (nothrow) BackgroundSizeStyleValue(size_x, size_y));
    }
    virtual ~BackgroundSizeStyleValue() override;

    LengthPercentage size_x() const { return m_properties.size_x; }
    LengthPercentage size_y() const { return m_properties.size_y; }

    virtual String to_string() const override;

    bool properties_equal(BackgroundSizeStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    BackgroundSizeStyleValue(LengthPercentage size_x, LengthPercentage size_y);

    struct Properties {
        LengthPercentage size_x;
        LengthPercentage size_y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
