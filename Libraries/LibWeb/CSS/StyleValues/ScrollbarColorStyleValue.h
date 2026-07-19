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
    static ValueComparingNonnullRefPtr<ScrollbarColorStyleValue const> create(NonnullRefPtr<StyleValue const> thumb_color, NonnullRefPtr<StyleValue const> track_color);
    virtual ~ScrollbarColorStyleValue() override = default;

    void serialize(StringBuilder&, SerializationMode) const;
    bool properties_equal(ScrollbarColorStyleValue const& other) const { return thumb_color() == other.thumb_color() && track_color() == other.track_color(); }

    ValueComparingNonnullRefPtr<StyleValue const> thumb_color() const { return *static_cast<StyleValue const*>(m_value->scrollbar_color.thumb_color.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> track_color() const { return *static_cast<StyleValue const*>(m_value->scrollbar_color.track_color.pointer); }

private:
    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    explicit ScrollbarColorStyleValue(NonnullRefPtr<StyleValue const> thumb_color, NonnullRefPtr<StyleValue const> track_color)
        : StyleValueWithDefaultOperators(Type::ScrollbarColor, StyleValueFFI::rust_style_value_create_scrollbar_color(&thumb_color.leak_ref(), &track_color.leak_ref()))
    {
    }
};

}
