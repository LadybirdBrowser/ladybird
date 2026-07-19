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

    void serialize(StringBuilder&, SerializationMode) const;
    virtual ~LinearGradientStyleValue() override = default;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const& other) const;

    Vector<ColorStopListElement> color_stop_list() const
    {
        auto const& list = m_value->linear_gradient.color_stop_list;
        return color_stops_from_rust_data(list.pointer, list.length);
    }

    GradientDirection direction() const
    {
        auto const& data = m_value->linear_gradient;
        if (data.has_direction_value)
            return NonnullRefPtr { *static_cast<StyleValue const*>(data.direction_value.pointer) };
        return static_cast<SideOrCorner>(data.side_or_corner);
    }

    // FIXME: This (and the any_non_legacy code in the constructor) is duplicated in the separate gradient classes,
    // should this logic be pulled into some kind of GradientStyleValue superclass?
    // It could also contain the "gradient related things" currently in AbstractImageStyleValue.h
    ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method() const
    {
        if (auto interpolation_method_value = color_interpolation_method_value())
            return interpolation_method_value->as_color_interpolation_method().color_interpolation_method();

        return ColorInterpolationMethodStyleValue::default_color_interpolation_method(gradient_color_syntax());
    }

    bool is_repeating() const { return m_value->linear_gradient.repeating; }

    float angle_degrees(CSSPixelSize gradient_size) const;

    void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;

    bool is_paintable(DOM::Document const&) const override { return true; }
    void paint(DisplayListRecordingContext& context, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

private:
    LinearGradientStyleValue(GradientDirection direction, Vector<ColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating, ValueComparingRefPtr<StyleValue const> color_interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::LinearGradient, make_linear_gradient_data(direction, color_stop_list, type, repeating, color_interpolation_method, color_syntax))
    {
    }

    static StyleValueFFI::StyleValueData* make_linear_gradient_data(GradientDirection const&, Vector<ColorStopListElement> const&, GradientType, GradientRepeating, RefPtr<StyleValue const> const&, ColorSyntax);

    ValueComparingRefPtr<StyleValue const> color_interpolation_method_value() const { return static_cast<StyleValue const*>(m_value->linear_gradient.color_interpolation_method.pointer); }
    GradientType gradient_type() const { return static_cast<GradientType>(m_value->linear_gradient.gradient_type); }
    ColorSyntax gradient_color_syntax() const { return static_cast<ColorSyntax>(m_value->linear_gradient.color_syntax); }

    mutable Optional<CSSPixelSize> m_resolved_size;
    mutable Optional<Painting::LinearGradientData> m_resolved;
};

}
