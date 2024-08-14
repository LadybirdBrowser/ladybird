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

namespace Web::CSS {

class LengthStyleValue : public StyleValueWithDefaultOperators<LengthStyleValue> {
public:
    static ValueComparingNonnullRefPtr<LengthStyleValue> create(Length const&);
    virtual ~LengthStyleValue() override = default;

    Length const& length() const { return m_length; }

    virtual String to_string() const override { return m_length.to_string(); }
    virtual ValueComparingNonnullRefPtr<CSSStyleValue const> absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const override;

    bool properties_equal(LengthStyleValue const& other) const { return m_length == other.m_length; }

private:
    explicit LengthStyleValue(Length const& length)
        : StyleValueWithDefaultOperators(Type::Length)
        , m_length(length)
    {
    }

    Length m_length;
};

}
