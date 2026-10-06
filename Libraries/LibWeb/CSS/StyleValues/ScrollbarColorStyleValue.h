/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ScrollbarColorStyleValue final : public StyleValueWithDefaultOperators<ScrollbarColorStyleValue> {
public:
    virtual ~ScrollbarColorStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> thumb_color() const { return wrap_rust_child(m_value->scrollbar_color.thumb_color); }
    ValueComparingNonnullRefPtr<StyleValue const> track_color() const { return wrap_rust_child(m_value->scrollbar_color.track_color); }

private:
    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;

    explicit ScrollbarColorStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::ScrollbarColor, data)
    {
    }
};

}
