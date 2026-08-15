/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

enum class ShadowPlacement {
    Outer,
    Inner,
};

class ShadowStyleValue final : public StyleValueWithDefaultOperators<ShadowStyleValue> {
public:
    enum class ShadowType : u8 {
        // none | <shadow>#
        Normal,
        // none | [ <color>? && <length>{2,3} ]#
        Text
    };

    static ValueComparingNonnullRefPtr<ShadowStyleValue const> create(
        ShadowType shadow_type,
        ValueComparingRefPtr<StyleValue const> color,
        ValueComparingNonnullRefPtr<StyleValue const> offset_x,
        ValueComparingNonnullRefPtr<StyleValue const> offset_y,
        ValueComparingRefPtr<StyleValue const> blur_radius,
        ValueComparingRefPtr<StyleValue const> spread_distance,
        ShadowPlacement placement)
    {
        return adopt_ref(*new (nothrow) ShadowStyleValue(shadow_type, move(color), move(offset_x), move(offset_y), move(blur_radius), move(spread_distance), placement));
    }
    virtual ~ShadowStyleValue() override = default;

    ShadowType shadow_type() const { return static_cast<ShadowType>(m_value->shadow.shadow_type); }
    ValueComparingNonnullRefPtr<StyleValue const> color() const;
    ValueComparingRefPtr<StyleValue const> color_or_null() const { return wrap_rust_child_or_null(m_value->shadow.color); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return wrap_rust_child(m_value->shadow.offset_x); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return wrap_rust_child(m_value->shadow.offset_y); }
    ValueComparingNonnullRefPtr<StyleValue const> blur_radius() const;
    ValueComparingRefPtr<StyleValue const> blur_radius_or_null() const { return wrap_rust_child_or_null(m_value->shadow.blur_radius); }
    ValueComparingNonnullRefPtr<StyleValue const> spread_distance() const;
    ValueComparingRefPtr<StyleValue const> spread_distance_or_null() const { return wrap_rust_child_or_null(m_value->shadow.spread_distance); }
    ShadowPlacement placement() const { return static_cast<ShadowPlacement>(m_value->shadow.placement); }

private:
    ShadowStyleValue(
        ShadowType shadow_type,
        ValueComparingRefPtr<StyleValue const> color,
        ValueComparingNonnullRefPtr<StyleValue const> offset_x,
        ValueComparingNonnullRefPtr<StyleValue const> offset_y,
        ValueComparingRefPtr<StyleValue const> blur_radius,
        ValueComparingRefPtr<StyleValue const> spread_distance,
        ShadowPlacement placement)
        : StyleValueWithDefaultOperators(Type::Shadow, make_shadow_data(shadow_type, color, offset_x, offset_y, blur_radius, spread_distance, placement))
    {
    }

    explicit ShadowStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Shadow, data)
    {
    }

    static StyleValueFFI::StyleValueData const* make_shadow_data(ShadowType shadow_type, ValueComparingRefPtr<StyleValue const> const& color, ValueComparingNonnullRefPtr<StyleValue const> const& offset_x, ValueComparingNonnullRefPtr<StyleValue const> const& offset_y, ValueComparingRefPtr<StyleValue const> const& blur_radius, ValueComparingRefPtr<StyleValue const> const& spread_distance, ShadowPlacement placement)
    {
        auto retain = [](StyleValue const* value) {
            return value ? StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) : nullptr;
        };
        return StyleValueFFI::rust_style_value_create_shadow(to_underlying(shadow_type), retain(color.ptr()), retain(offset_x.ptr()), retain(offset_y.ptr()), retain(blur_radius.ptr()), retain(spread_distance.ptr()), to_underlying(placement));
    }

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
};

}
