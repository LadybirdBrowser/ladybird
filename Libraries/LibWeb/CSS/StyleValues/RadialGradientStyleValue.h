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
#include <LibWeb/Export.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::CSS {

class WEB_API RadialGradientStyleValue final : public AbstractImageStyleValue {
public:
    enum class EndingShape {
        Circle,
        Ellipse
    };

    static ValueComparingNonnullRefPtr<RadialGradientStyleValue const> create(EndingShape ending_shape, NonnullRefPtr<StyleValue const> size, ValueComparingNonnullRefPtr<PositionStyleValue const> position, Vector<ColorStopListElement> color_stop_list, GradientRepeating repeating, RefPtr<StyleValue const> color_interpolation_method)
    {
        VERIFY(!color_stop_list.is_empty());
        bool any_non_legacy = color_stop_list.find_first_index_if([](auto const& stop) { return !stop.color_stop.color->is_keyword() && stop.color_stop.color->as_color().color_syntax() == ColorSyntax::Modern; }).has_value();
        return adopt_ref(*new (nothrow) RadialGradientStyleValue(ending_shape, move(size), move(position), move(color_stop_list), repeating, move(color_interpolation_method), any_non_legacy ? ColorSyntax::Modern : ColorSyntax::Legacy));
    }

    void serialize(StringBuilder&, SerializationMode) const;

    void paint(DisplayListRecordingContext&, DOM::Document const&, DevicePixelRect const& dest_rect, CSS::ImageRendering) const override;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool equals(StyleValue const& other) const;

    Vector<ColorStopListElement> color_stop_list() const
    {
        auto const& list = m_value->radial_gradient.color_stop_list;
        return color_stops_from_rust_data(list.pointer, list.length);
    }

    ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method() const
    {
        if (auto interpolation_method_value = color_interpolation_method_value())
            return interpolation_method_value->as_color_interpolation_method().color_interpolation_method();

        return ColorInterpolationMethodStyleValue::default_color_interpolation_method(gradient_color_syntax());
    }

    bool is_paintable(DOM::Document const&) const override { return true; }

    void resolve_for_size(Layout::NodeWithStyle const&, CSSPixelSize) const override;

    CSSPixelSize resolve_size(CSSPixelPoint, CSSPixelRect const&) const;

    bool is_repeating() const { return m_value->radial_gradient.repeating; }

    virtual ~RadialGradientStyleValue() override = default;

private:
    RadialGradientStyleValue(EndingShape ending_shape, NonnullRefPtr<StyleValue const> size, ValueComparingNonnullRefPtr<PositionStyleValue const> position, Vector<ColorStopListElement> color_stop_list, GradientRepeating repeating, ValueComparingRefPtr<StyleValue const> color_interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::RadialGradient, make_radial_gradient_data(ending_shape, size, position, color_stop_list, repeating, color_interpolation_method, color_syntax))
    {
    }

    static StyleValueFFI::StyleValueData* make_radial_gradient_data(EndingShape, NonnullRefPtr<StyleValue const> const&, NonnullRefPtr<PositionStyleValue const> const&, Vector<ColorStopListElement> const&, GradientRepeating, RefPtr<StyleValue const> const&, ColorSyntax);

    ValueComparingNonnullRefPtr<StyleValue const> size_value() const { return *static_cast<StyleValue const*>(m_value->radial_gradient.size.pointer); }
    ValueComparingNonnullRefPtr<PositionStyleValue const> position_value() const { return *static_cast<PositionStyleValue const*>(m_value->radial_gradient.position.pointer); }
    EndingShape ending_shape() const { return static_cast<EndingShape>(m_value->radial_gradient.ending_shape); }
    ColorSyntax gradient_color_syntax() const { return static_cast<ColorSyntax>(m_value->radial_gradient.color_syntax); }

    ValueComparingRefPtr<StyleValue const> color_interpolation_method_value() const { return static_cast<StyleValue const*>(m_value->radial_gradient.color_interpolation_method.pointer); }

    mutable Optional<CSSPixelSize> m_resolved_size;

    struct ResolvedData {
        Painting::RadialGradientData data;
        CSSPixelSize gradient_size;
        CSSPixelPoint center;
    };
    mutable Optional<ResolvedData> m_resolved;
};

}
