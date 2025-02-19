/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::CSS {

// Note: The sides must be before the corners in this enum (as this order is used in parsing).
enum class SideOrCorner {
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

class LinearGradientStyleValue final : public AbstractImageStyleValue {
public:
    using GradientDirection = Variant<Angle, SideOrCorner>;

    enum class GradientType {
        Standard,
        WebKit
    };

    static ValueComparingNonnullRefPtr<LinearGradientStyleValue> create(GradientDirection direction, Vector<LinearColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating, Optional<InterpolationMethod> interpolation_method)
    {
        VERIFY(!color_stop_list.is_empty());
        bool any_non_legacy = color_stop_list.find_first_index_if([](auto const& stop) { return !stop.color_stop.color->is_keyword() && stop.color_stop.color->as_color().color_syntax() == ColorSyntax::Modern; }).has_value();
        return adopt_ref(*new (nothrow) LinearGradientStyleValue(direction, move(color_stop_list), type, repeating, interpolation_method, any_non_legacy ? ColorSyntax::Modern : ColorSyntax::Legacy));
    }

    virtual String to_string(SerializationMode) const override;
    virtual ~LinearGradientStyleValue() override = default;
    virtual bool equals(CSSStyleValue const& other) const override;

    Vector<LinearColorStopListElement> const& color_stop_list() const
    {
        return m_properties.color_stop_list;
    }

    // FIXME: This (and the any_non_legacy code in the constructor) is duplicated in the separate gradient classes,
    // should this logic be pulled into some kind of GradientStyleValue superclass?
    // It could also contain the "gradient related things" currently in AbstractImageStyleValue.h
    InterpolationMethod interpolation_method() const
    {
        if (m_properties.interpolation_method.has_value())
            return m_properties.interpolation_method.value();

        return InterpolationMethod { .color_space = InterpolationMethod::default_color_space(m_properties.color_syntax) };
    }

    bool is_repeating() const { return m_properties.repeating == GradientRepeating::Yes; }

    float angle_degrees(CSSPixelSize gradient_size) const;

    void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;

    bool is_paintable() const override { return true; }
    void paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

private:
    LinearGradientStyleValue(GradientDirection direction, Vector<LinearColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating, Optional<InterpolationMethod> interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::LinearGradient)
        , m_properties { .direction = direction, .color_stop_list = move(color_stop_list), .gradient_type = type, .repeating = repeating, .interpolation_method = interpolation_method, .color_syntax = color_syntax }
    {
    }

    struct Properties {
        GradientDirection direction;
        Vector<LinearColorStopListElement> color_stop_list;
        GradientType gradient_type;
        GradientRepeating repeating;
        Optional<InterpolationMethod> interpolation_method;
        ColorSyntax color_syntax;
        bool operator==(Properties const&) const = default;
    } m_properties;

    struct ResolvedDataCacheKey {
        Length::ResolutionContext length_resolution_context;
        CSSPixelSize size;
        bool operator==(ResolvedDataCacheKey const&) const = default;
    };
    mutable Optional<ResolvedDataCacheKey> m_resolved_data_cache_key;
    mutable Optional<Painting::LinearGradientData> m_resolved;
};

}
