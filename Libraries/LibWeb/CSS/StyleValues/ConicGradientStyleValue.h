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

    bool is_paintable(GC::Ptr<HTML::DecodedImageData>) const override { return true; }

    Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&) const override;

    virtual ~ConicGradientStyleValue() override = default;

private:
    friend class StyleValue;

    ConicGradientStyleValue(ValueComparingRefPtr<StyleValue const> from_angle, ValueComparingNonnullRefPtr<PositionStyleValue const> position, Vector<ColorStopListElement> color_stop_list, GradientRepeating repeating, ValueComparingRefPtr<StyleValue const> color_interpolation_method, ColorSyntax color_syntax)
        : AbstractImageStyleValue(Type::ConicGradient, make_conic_gradient_data(from_angle, position, color_stop_list, repeating, color_interpolation_method, color_syntax))
    {
    }

    explicit ConicGradientStyleValue(StyleValueFFI::StyleValueData const*);

    static StyleValueFFI::StyleValueData const* make_conic_gradient_data(RefPtr<StyleValue const> const&, NonnullRefPtr<PositionStyleValue const> const&, Vector<ColorStopListElement> const&, GradientRepeating, RefPtr<StyleValue const> const&, ColorSyntax);
};

}
