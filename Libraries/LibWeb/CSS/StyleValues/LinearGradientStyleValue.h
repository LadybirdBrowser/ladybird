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
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorInterpolationMethodStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
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
    using GradientDirection = Variant<NonnullRefPtr<StyleValue const>, SideOrCorner>;

    enum class GradientType {
        Standard,
        WebKit
    };

    static ValueComparingNonnullRefPtr<LinearGradientStyleValue const> create(GradientDirection direction, Vector<ColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating, RefPtr<StyleValue const> color_interpolation_method)
    {
        VERIFY(!color_stop_list.is_empty());
        bool any_non_legacy = color_stop_list.find_first_index_if([](auto const& stop) { return !stop.color_stop.color->is_keyword() && stop.color_stop.color->as_color().color_syntax() == ColorSyntax::Modern; }).has_value();
        return adopt_ref(*new (nothrow) LinearGradientStyleValue(move(direction), move(color_stop_list), type, repeating, move(color_interpolation_method), any_non_legacy ? ColorSyntax::Modern : ColorSyntax::Legacy));
    }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual ~LinearGradientStyleValue() override = default;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual bool equals(StyleValue const& other) const override;

    virtual bool is_computationally_independent() const override
    {
        auto is_direction_computationally_independent = m_properties.direction.visit(
            [](NonnullRefPtr<StyleValue const> const& value) { return value->is_computationally_independent(); },
            [](SideOrCorner) { return true; });

        return is_direction_computationally_independent
            && all_of(m_properties.color_stop_list, [&](auto const& stop) { return stop.color_stop.color->is_computationally_independent(); })
            && (!m_properties.color_interpolation_method || m_properties.color_interpolation_method->is_computationally_independent());
    }

    Vector<ColorStopListElement> const& color_stop_list() const
    {
        return m_properties.color_stop_list;
    }

    // FIXME: This (and the any_non_legacy code in the constructor) is duplicated in the separate gradient classes,
    // should this logic be pulled into some kind of GradientStyleValue superclass?
    // It could also contain the "gradient related things" currently in AbstractImageStyleValue.h
    ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method() const
    {
        if (m_properties.color_interpolation_method)
            return m_properties.color_interpolation_method->as_color_interpolation_method().color_interpolation_method();

        return ColorInterpolationMethodStyleValue::default_color_interpolation_method(m_properties.color_syntax);
    }

    bool is_repeating() const { return m_properties.repeating == GradientRepeating::Yes; }

    float angle_degrees(CSSPixelSize gradient_size) const;

    void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;

    bool is_paintable() const override { return true; }
    void paint(DisplayListRecordingContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

private:
    LinearGradientStyleValue(GradientDirection direction, Vector<ColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating, ValueComparingRefPtr<StyleValue const> color_interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::LinearGradient)
        , m_properties { .direction = move(direction), .color_stop_list = move(color_stop_list), .gradient_type = type, .repeating = repeating, .color_interpolation_method = move(color_interpolation_method), .color_syntax = color_syntax }
    {
    }

    struct Properties {
        GradientDirection direction;
        Vector<ColorStopListElement> color_stop_list;
        GradientType gradient_type;
        GradientRepeating repeating;
        ValueComparingRefPtr<StyleValue const> color_interpolation_method;
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
