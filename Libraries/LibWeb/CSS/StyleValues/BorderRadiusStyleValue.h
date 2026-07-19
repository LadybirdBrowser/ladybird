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
    static ValueComparingNonnullRefPtr<BorderRadiusStyleValue const> create_zero()
    {
        return create(LengthStyleValue::create(Length::make_px(0)), LengthStyleValue::create(Length::make_px(0)));
    }

    static ValueComparingNonnullRefPtr<BorderRadiusStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> const& horizontal_radius, ValueComparingNonnullRefPtr<StyleValue const> const& vertical_radius)
    {
        return adopt_ref(*new (nothrow) BorderRadiusStyleValue(horizontal_radius, vertical_radius));
    }
    virtual ~BorderRadiusStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> horizontal_radius() const { return *static_cast<StyleValue const*>(m_value->border_radius.horizontal_radius.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> vertical_radius() const { return *static_cast<StyleValue const*>(m_value->border_radius.vertical_radius.pointer); }
    bool is_elliptical() const { return m_value->border_radius.is_elliptical; }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(BorderRadiusStyleValue const& other) const
    {
        return is_elliptical() == other.is_elliptical()
            && horizontal_radius() == other.horizontal_radius()
            && vertical_radius() == other.vertical_radius();
    }

private:
    BorderRadiusStyleValue(ValueComparingNonnullRefPtr<StyleValue const> const& horizontal_radius, ValueComparingNonnullRefPtr<StyleValue const> const& vertical_radius)
        : StyleValueWithDefaultOperators(Type::BorderRadius, make_border_radius_data(horizontal_radius, vertical_radius))
    {
    }

    static StyleValueFFI::StyleValueData* make_border_radius_data(ValueComparingNonnullRefPtr<StyleValue const> const& horizontal_radius, ValueComparingNonnullRefPtr<StyleValue const> const& vertical_radius)
    {
        // The Rust allocation takes ownership of one strong reference to each radius.
        return StyleValueFFI::rust_style_value_create_border_radius(
            horizontal_radius != vertical_radius,
            retain_style_value_for_rust(horizontal_radius.ptr()), retain_style_value_for_rust(vertical_radius.ptr()));
    }

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
};

}
