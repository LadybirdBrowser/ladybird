/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// NOTE: This is not used for identifier sizes, like `cover` and `contain`.
class BackgroundSizeStyleValue final : public StyleValueWithDefaultOperators<BackgroundSizeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BackgroundSizeStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> size_x, ValueComparingNonnullRefPtr<StyleValue const> size_y)
    {
        return adopt_ref(*new (nothrow) BackgroundSizeStyleValue(move(size_x), move(size_y)));
    }
    virtual ~BackgroundSizeStyleValue() override;

    ValueComparingNonnullRefPtr<StyleValue const> size_x() const { return m_properties.size_x; }
    ValueComparingNonnullRefPtr<StyleValue const> size_y() const { return m_properties.size_y; }

    virtual String to_string(SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(BackgroundSizeStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    BackgroundSizeStyleValue(ValueComparingNonnullRefPtr<StyleValue const> size_x, ValueComparingNonnullRefPtr<StyleValue const> size_y);

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValue const> size_x;
        ValueComparingNonnullRefPtr<StyleValue const> size_y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
