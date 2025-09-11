/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API LengthStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<LengthStyleValue const> create(Length const&);
    virtual ~LengthStyleValue() override = default;

    Length const& length() const { return m_length; }
    virtual double raw_value() const override { return m_length.raw_value(); }
    virtual FlyString unit_name() const override { return m_length.unit_name(); }

    virtual String to_string(SerializationMode serialization_mode) const override { return m_length.to_string(serialization_mode); }
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const override;

    bool equals(StyleValue const& other) const override;

private:
    explicit LengthStyleValue(Length const& length)
        : DimensionStyleValue(Type::Length)
        , m_length(length)
    {
    }

    Length m_length;
};

}
