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

    ShadowType shadow_type() const { return m_properties.shadow_type; }
    ValueComparingNonnullRefPtr<StyleValue const> color() const;
    ValueComparingNonnullRefPtr<StyleValue const> offset_x() const { return m_properties.offset_x; }
    ValueComparingNonnullRefPtr<StyleValue const> offset_y() const { return m_properties.offset_y; }
    ValueComparingNonnullRefPtr<StyleValue const> blur_radius() const;
    ValueComparingNonnullRefPtr<StyleValue const> spread_distance() const;
    ShadowPlacement placement() const { return m_properties.placement; }

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(ShadowStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    ShadowStyleValue(
        ShadowType shadow_type,
        ValueComparingRefPtr<StyleValue const> color,
        ValueComparingNonnullRefPtr<StyleValue const> offset_x,
        ValueComparingNonnullRefPtr<StyleValue const> offset_y,
        ValueComparingRefPtr<StyleValue const> blur_radius,
        ValueComparingRefPtr<StyleValue const> spread_distance,
        ShadowPlacement placement)
        : StyleValueWithDefaultOperators(Type::Shadow)
        , m_properties {
            .shadow_type = shadow_type,
            .color = move(color),
            .offset_x = move(offset_x),
            .offset_y = move(offset_y),
            .blur_radius = move(blur_radius),
            .spread_distance = move(spread_distance),
            .placement = placement
        }
    {
    }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const override;

    struct Properties {
        ShadowType shadow_type;
        ValueComparingRefPtr<StyleValue const> color;
        ValueComparingNonnullRefPtr<StyleValue const> offset_x;
        ValueComparingNonnullRefPtr<StyleValue const> offset_y;
        ValueComparingRefPtr<StyleValue const> blur_radius;
        ValueComparingRefPtr<StyleValue const> spread_distance;
        ShadowPlacement placement;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
