/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class LengthStyleValue final : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<LengthStyleValue> create(Length const&);
    virtual ~LengthStyleValue() override = default;

    Length const& length() const { return m_length; }
    virtual double value() const override { return m_length.raw_value(); }
    virtual StringView unit() const override { return m_length.unit_name(); }

    virtual String to_string() const override { return m_length.to_string(); }
    virtual ValueComparingNonnullRefPtr<CSSStyleValue const> absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const override;

    bool equals(CSSStyleValue const& other) const override;

private:
    explicit LengthStyleValue(Length const& length)
        : CSSUnitValue(Type::Length)
        , m_length(length)
    {
    }

    Length m_length;
};

}
