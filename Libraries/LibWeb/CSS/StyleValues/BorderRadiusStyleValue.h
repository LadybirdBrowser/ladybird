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
    virtual ~BorderRadiusStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> horizontal_radius() const { return wrap_rust_child(m_value->border_radius.horizontal_radius); }
    ValueComparingNonnullRefPtr<StyleValue const> vertical_radius() const { return wrap_rust_child(m_value->border_radius.vertical_radius); }
    bool is_elliptical() const { return m_value->border_radius.is_elliptical; }

private:
    explicit BorderRadiusStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::BorderRadius, data)
    {
    }

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
};

}
