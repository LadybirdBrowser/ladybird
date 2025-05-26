/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>

namespace Web::CSS {

class ScrollbarColorStyleValue final : public StyleValueWithDefaultOperators<ScrollbarColorStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ScrollbarColorStyleValue const> create(NonnullRefPtr<CSSStyleValue const> thumb_color, NonnullRefPtr<CSSStyleValue const> track_color);
    virtual ~ScrollbarColorStyleValue() override = default;

    virtual String to_string(SerializationMode) const override;
    bool properties_equal(ScrollbarColorStyleValue const& other) const { return m_thumb_color == other.m_thumb_color && m_track_color == other.m_track_color; }

    NonnullRefPtr<CSSStyleValue const> thumb_color() const { return m_thumb_color; }
    NonnullRefPtr<CSSStyleValue const> track_color() const { return m_track_color; }

private:
    explicit ScrollbarColorStyleValue(NonnullRefPtr<CSSStyleValue const> thumb_color, NonnullRefPtr<CSSStyleValue const> track_color)
        : StyleValueWithDefaultOperators(Type::ScrollbarColor)
        , m_thumb_color(move(thumb_color))
        , m_track_color(move(track_color))
    {
    }

    NonnullRefPtr<CSSStyleValue const> m_thumb_color;
    NonnullRefPtr<CSSStyleValue const> m_track_color;
};

}
