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

class BorderRadiusStyleValue final : public StyleValueWithDefaultOperators<BorderRadiusStyleValue> {
public:
    static ValueComparingNonnullRefPtr<BorderRadiusStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> const& horizontal_radius, ValueComparingNonnullRefPtr<StyleValue const> const& vertical_radius)
    {
        return adopt_ref(*new (nothrow) BorderRadiusStyleValue(horizontal_radius, vertical_radius));
    }
    virtual ~BorderRadiusStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> const& horizontal_radius() const { return m_properties.horizontal_radius; }
    ValueComparingNonnullRefPtr<StyleValue const> const& vertical_radius() const { return m_properties.vertical_radius; }
    bool is_elliptical() const { return m_properties.is_elliptical; }

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(BorderRadiusStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    BorderRadiusStyleValue(ValueComparingNonnullRefPtr<StyleValue const> const& horizontal_radius, ValueComparingNonnullRefPtr<StyleValue const> const& vertical_radius)
        : StyleValueWithDefaultOperators(Type::BorderRadius)
        , m_properties { .is_elliptical = horizontal_radius != vertical_radius, .horizontal_radius = horizontal_radius, .vertical_radius = vertical_radius }
    {
    }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    struct Properties {
        bool is_elliptical;
        ValueComparingNonnullRefPtr<StyleValue const> horizontal_radius;
        ValueComparingNonnullRefPtr<StyleValue const> vertical_radius;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
