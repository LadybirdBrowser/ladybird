/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csscolorvalue
class CSSColorValue : public StyleValueWithDefaultOperators<CSSColorValue> {
public:
    static ValueComparingNonnullRefPtr<CSSColorValue> create(Color color);
    virtual ~CSSColorValue() override = default;

    Color color() const { return m_color; }
    virtual String to_string() const override;
    virtual bool has_color() const override { return true; }
    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override { return m_color; }

    bool properties_equal(CSSColorValue const& other) const { return m_color == other.m_color; }

private:
    explicit CSSColorValue(Color color)
        : StyleValueWithDefaultOperators(Type::Color)
        , m_color(color)
    {
    }

    Color m_color;
};

}
