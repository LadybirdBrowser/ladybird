/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/CSS/Length.h>
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
    ValueComparingRefPtr<StyleValue const> color_or_null() const { return static_cast<StyleValue const*>(m_value->shadow.color.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return *static_cast<StyleValue const*>(m_value->shadow.offset_x.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return *static_cast<StyleValue const*>(m_value->shadow.offset_y.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> blur_radius() const;
    ValueComparingRefPtr<StyleValue const> blur_radius_or_null() const { return static_cast<StyleValue const*>(m_value->shadow.blur_radius.pointer); }
    ValueComparingNonnullRefPtr<StyleValue const> spread_distance() const;
    ValueComparingRefPtr<StyleValue const> spread_distance_or_null() const { return static_cast<StyleValue const*>(m_value->shadow.spread_distance.pointer); }
    ShadowPlacement placement() const { return static_cast<ShadowPlacement>(m_value->shadow.placement); }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(ShadowStyleValue const& other) const { return shadow_type() == other.shadow_type() && color_or_null() == other.color_or_null() && offset_x() == other.offset_x() && offset_y() == other.offset_y() && blur_radius_or_null() == other.blur_radius_or_null() && spread_distance_or_null() == other.spread_distance_or_null() && placement() == other.placement(); }

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

    static StyleValueFFI::StyleValueData* make_shadow_data(ShadowType shadow_type, ValueComparingRefPtr<StyleValue const> const& color, ValueComparingNonnullRefPtr<StyleValue const> const& offset_x, ValueComparingNonnullRefPtr<StyleValue const> const& offset_y, ValueComparingRefPtr<StyleValue const> const& blur_radius, ValueComparingRefPtr<StyleValue const> const& spread_distance, ShadowPlacement placement)
    {
        // The Rust allocation takes ownership of one strong reference to each non-null value.
        offset_x->ref();
        offset_y->ref();
        if (color)
            color->ref();
        if (blur_radius)
            blur_radius->ref();
        if (spread_distance)
            spread_distance->ref();
        return StyleValueFFI::rust_style_value_create_shadow(to_underlying(shadow_type), color.ptr(), offset_x.ptr(), offset_y.ptr(), blur_radius.ptr(), spread_distance.ptr(), to_underlying(placement));
    }

    // NB: StyleValue dispatches operations by type tag, so it may call private impls.
    friend class StyleValue;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
};

}
