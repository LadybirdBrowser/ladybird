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
#include <LibWeb/CSS/EdgeRect.h>

namespace Web::CSS {

class RectStyleValue : public StyleValueWithDefaultOperators<RectStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RectStyleValue> create(EdgeRect rect);
    virtual ~RectStyleValue() override = default;

    EdgeRect rect() const { return m_rect; }
    virtual String to_string() const override;

    bool properties_equal(RectStyleValue const& other) const { return m_rect == other.m_rect; }

private:
    explicit RectStyleValue(EdgeRect rect)
        : StyleValueWithDefaultOperators(Type::Rect)
        , m_rect(move(rect))
    {
    }

    EdgeRect m_rect;
};

}
