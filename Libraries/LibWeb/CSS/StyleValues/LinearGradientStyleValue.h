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

    virtual ~LinearGradientStyleValue() override = default;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    Vector<ColorStopListElement> color_stop_list() const
    {
        auto const& list = m_value->linear_gradient.color_stop_list;
        return color_stops_from_rust_data(list.pointer, list.length);
    }

    GradientDirection direction() const
    {
        auto const& gradient = m_value->linear_gradient;
        if (!gradient.has_direction_value)
            return static_cast<SideOrCorner>(gradient.side_or_corner);
        return NonnullRefPtr<StyleValue const> { wrap_rust_child(gradient.direction_value) };
    }

    bool is_repeating() const { return m_value->linear_gradient.repeating; }

    Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&) const override;

    bool is_paintable(GC::Ptr<HTML::DecodedImageData>) const override { return true; }

private:
    friend class StyleValue;

    LinearGradientStyleValue(GradientDirection direction, Vector<ColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating, ValueComparingRefPtr<StyleValue const> color_interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::LinearGradient, make_linear_gradient_data(direction, color_stop_list, type, repeating, color_interpolation_method, color_syntax))
    {
    }

    explicit LinearGradientStyleValue(StyleValueFFI::StyleValueData const*);

    static StyleValueFFI::StyleValueData const* make_linear_gradient_data(GradientDirection const&, Vector<ColorStopListElement> const&, GradientType, GradientRepeating, RefPtr<StyleValue const> const&, ColorSyntax);

    ValueComparingRefPtr<StyleValue const> color_interpolation_method_value() const { return wrap_rust_child_or_null(m_value->linear_gradient.color_interpolation_method); }
    GradientType gradient_type() const { return static_cast<GradientType>(m_value->linear_gradient.gradient_type); }
    ColorSyntax gradient_color_syntax() const { return static_cast<ColorSyntax>(m_value->linear_gradient.color_syntax); }
};

}
