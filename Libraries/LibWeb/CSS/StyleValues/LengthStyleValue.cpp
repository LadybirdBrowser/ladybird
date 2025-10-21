/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LengthStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<LengthStyleValue const> LengthStyleValue::create(Length const& length)
{
    if (length.is_px()) {
        if (length.raw_value() == 0) {
            static auto value = adopt_ref(*new (nothrow) LengthStyleValue(CSS::Length::make_px(0)));
            return value;
        }
        if (length.raw_value() == 1) {
            static auto value = adopt_ref(*new (nothrow) LengthStyleValue(CSS::Length::make_px(1)));
            return value;
        }
    }
    return adopt_ref(*new (nothrow) LengthStyleValue(length));
}

ValueComparingNonnullRefPtr<StyleValue const> LengthStyleValue::absolutized(ComputationContext const& computation_context) const
{
    if (auto length = m_length.absolutize(computation_context.length_resolution_context.viewport_rect, computation_context.length_resolution_context.font_metrics, computation_context.length_resolution_context.root_font_metrics); length.has_value())
        return LengthStyleValue::create(length.release_value());
    return *this;
}

bool LengthStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_length = other.as_length();
    return m_length == other_length.m_length;
}

}
