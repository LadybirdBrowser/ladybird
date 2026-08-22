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

    ValueComparingNonnullRefPtr<StyleValue const> horizontal_radius() const { return wrap_rust_child(m_value->border_radius.horizontal_radius); }
    ValueComparingNonnullRefPtr<StyleValue const> vertical_radius() const { return wrap_rust_child(m_value->border_radius.vertical_radius); }
    bool is_elliptical() const { return m_value->border_radius.is_elliptical; }

private:
    explicit BorderRadiusStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::BorderRadius, data)
    {
    }

    BorderRadiusStyleValue(ValueComparingNonnullRefPtr<StyleValue const> const& horizontal_radius, ValueComparingNonnullRefPtr<StyleValue const> const& vertical_radius)
        : StyleValueWithDefaultOperators(Type::BorderRadius, StyleValueFFI::rust_style_value_create_border_radius(horizontal_radius != vertical_radius, StyleValueFFI::rust_style_value_retain(horizontal_radius->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(vertical_radius->rust_style_value_data())))
    {
    }

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
};

}
