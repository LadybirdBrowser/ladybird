/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorInterpolationMethodStyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::CSS {

class WEB_API ConicGradientStyleValue final : public AbstractImageStyleValue {
public:
    static ValueComparingNonnullRefPtr<ConicGradientStyleValue const> create(ValueComparingRefPtr<StyleValue const> from_angle, ValueComparingNonnullRefPtr<PositionStyleValue const> position, Vector<ColorStopListElement> color_stop_list, GradientRepeating repeating, RefPtr<StyleValue const> color_interpolation_method)
    {
        VERIFY(!color_stop_list.is_empty());
        bool any_non_legacy = color_stop_list.find_first_index_if([](auto const& stop) { return !stop.color_stop.color->is_keyword() && stop.color_stop.color->as_color().color_syntax() == ColorSyntax::Modern; }).has_value();
        return adopt_ref(*new (nothrow) ConicGradientStyleValue(move(from_angle), move(position), move(color_stop_list), repeating, move(color_interpolation_method), any_non_legacy ? ColorSyntax::Modern : ColorSyntax::Legacy));
    }
    static ValueComparingNonnullRefPtr<ConicGradientStyleValue const> create(ValueComparingRefPtr<StyleValue const> from_angle, ValueComparingNonnullRefPtr<PositionStyleValue const> position, Vector<ColorStopListElement> color_stop_list, GradientRepeating repeating, RefPtr<StyleValue const> color_interpolation_method, ColorSyntax color_syntax)
    {
        VERIFY(!color_stop_list.is_empty());
        return adopt_ref(*new (nothrow) ConicGradientStyleValue(move(from_angle), move(position), move(color_stop_list), repeating, move(color_interpolation_method), color_syntax));
    }

    void serialize(StringBuilder&, SerializationMode) const;

    void paint(DisplayListRecordingContext&, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering) const override;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const& other) const;
    Vector<ColorStopListElement> color_stop_list() const
    {
        auto const& list = m_value->conic_gradient.color_stop_list;
        return color_stops_from_rust_data(list.pointer, list.length);
    }

    ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method() const
    {
        if (auto interpolation_method_value = color_interpolation_method_value())
            return interpolation_method_value->as_color_interpolation_method().color_interpolation_method();

        return ColorInterpolationMethodStyleValue::default_color_interpolation_method(gradient_color_syntax());
    }

    float angle_degrees() const;

    bool is_paintable(DOM::Document const&) const override { return true; }

    void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;

    virtual ~ConicGradientStyleValue() override = default;

    bool is_repeating() const { return m_value->conic_gradient.repeating; }

private:
    ConicGradientStyleValue(ValueComparingRefPtr<StyleValue const> from_angle, ValueComparingNonnullRefPtr<PositionStyleValue const> position, Vector<ColorStopListElement> color_stop_list, GradientRepeating repeating, ValueComparingRefPtr<StyleValue const> color_interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::ConicGradient, make_conic_gradient_data(from_angle, position, color_stop_list, repeating, color_interpolation_method, color_syntax))
    {
    }

    static StyleValueFFI::StyleValueData* make_conic_gradient_data(RefPtr<StyleValue const> const&, NonnullRefPtr<PositionStyleValue const> const&, Vector<ColorStopListElement> const&, GradientRepeating, RefPtr<StyleValue const> const&, ColorSyntax);

    ValueComparingRefPtr<StyleValue const> from_angle_value() const { return static_cast<StyleValue const*>(m_value->conic_gradient.from_angle.pointer); }
    ColorSyntax gradient_color_syntax() const { return static_cast<ColorSyntax>(m_value->conic_gradient.color_syntax); }
    ValueComparingNonnullRefPtr<PositionStyleValue const> position_value() const { return *static_cast<PositionStyleValue const*>(m_value->conic_gradient.position.pointer); }

    ValueComparingRefPtr<StyleValue const> color_interpolation_method_value() const { return static_cast<StyleValue const*>(m_value->conic_gradient.color_interpolation_method.pointer); }

    mutable Optional<CSSPixelSize> m_resolved_size;

    struct ResolvedData {
        Painting::ConicGradientData data;
        CSSPixelPoint position;
    };
    mutable Optional<ResolvedData> m_resolved;
};

}
