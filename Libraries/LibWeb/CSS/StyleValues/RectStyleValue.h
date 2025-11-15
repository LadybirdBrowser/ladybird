/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/EdgeRect.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RectStyleValue : public StyleValueWithDefaultOperators<RectStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RectStyleValue const> create(EdgeRect rect);
    virtual ~RectStyleValue() override = default;

    EdgeRect rect() const { return m_rect; }
    virtual String to_string(SerializationMode) const override;

    bool properties_equal(RectStyleValue const& other) const { return m_rect == other.m_rect; }

    static ValueComparingNonnullRefPtr<StyleValue const> neutral_value()
    {
        return RectStyleValue::create({ LengthOrAuto(Length::make_px(0)), LengthOrAuto(Length::make_px(0)), LengthOrAuto(Length::make_px(0)), LengthOrAuto(Length::make_px(0)) });
    }

private:
    explicit RectStyleValue(EdgeRect rect)
        : StyleValueWithDefaultOperators(Type::Rect)
        , m_rect(move(rect))
    {
    }

    EdgeRect m_rect;
};

}
