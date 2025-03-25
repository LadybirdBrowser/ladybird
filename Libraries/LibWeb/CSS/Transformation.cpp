/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Transformation.h"
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::CSS {

Transformation::Transformation(TransformFunction function, Vector<TransformValue>&& values)
    : m_function(function)
    , m_values(move(values))
{
}

ErrorOr<Gfx::FloatMatrix4x4> Transformation::to_matrix(Optional<Painting::PaintableBox const&> paintable_box) const
{
    auto count = m_values.size();
    auto value = [&](size_t index, CSSPixels const& reference_length = 0) -> ErrorOr<float> {
        CalculationResolutionContext context {};
        if (paintable_box.has_value())
            context.length_resolution_context = Length::ResolutionContext::for_layout_node(paintable_box->layout_node());

        return m_values[index].visit(
            [&](CSS::LengthPercentage const& value) -> ErrorOr<float> {
                context.percentage_basis = Length::make_px(reference_length);

                if (paintable_box.has_value())
                    return value.resolved(paintable_box->layout_node(), reference_length).to_px(paintable_box->layout_node()).to_float();
                if (value.is_length()) {
                    if (auto const& length = value.length(); length.is_absolute())
                        return length.absolute_length_to_px().to_float();
                }
                return Error::from_string_literal("Transform contains non absolute units");
            },
            [&](CSS::AngleOrCalculated const& value) -> ErrorOr<float> {
                if (auto resolved = value.resolved(context); resolved.has_value())
                    return resolved->to_radians();
                if (!value.is_calculated())
                    return value.value().to_radians();
                return Error::from_string_literal("Transform contains non absolute units");
            },
            [&](CSS::NumberPercentage const& value) -> ErrorOr<float> {
                if (value.is_percentage())
                    return value.percentage().as_fraction();
                if (value.is_number())
                    return value.number().value();
                if (value.is_calculated()) {
                    if (value.calculated()->resolves_to_number())
                        return value.calculated()->resolve_number(context).value();
                    if (value.calculated()->resolves_to_percentage())
                        return value.calculated()->resolve_percentage(context)->as_fraction();
                }
                return Error::from_string_literal("Transform contains non absolute units");
            });
    };

    CSSPixels width = 1;
    CSSPixels height = 1;
    if (paintable_box.has_value()) {
        auto reference_box = paintable_box->transform_box_rect();
        width = reference_box.width();
        height = reference_box.height();
    }

    switch (m_function) {
    case CSS::TransformFunction::Perspective:
        // https://drafts.csswg.org/css-transforms-2/#perspective
        // Count is zero when null parameter
        if (count == 1) {
            // FIXME: Add support for the 'perspective-origin' CSS property.
            auto distance = TRY(value(0));
            return Gfx::FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, -1 / (distance <= 0 ? 1 : distance), 1);
        } else {
            return Gfx::FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        }
        break;
    case CSS::TransformFunction::Matrix:
        if (count == 6)
            return Gfx::FloatMatrix4x4(TRY(value(0)), TRY(value(2)), 0, TRY(value(4)),
                TRY(value(1)), TRY(value(3)), 0, TRY(value(5)),
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::Matrix3d:
        if (count == 16)
            return Gfx::FloatMatrix4x4(TRY(value(0)), TRY(value(4)), TRY(value(8)), TRY(value(12)),
                TRY(value(1)), TRY(value(5)), TRY(value(9)), TRY(value(13)),
                TRY(value(2)), TRY(value(6)), TRY(value(10)), TRY(value(14)),
                TRY(value(3)), TRY(value(7)), TRY(value(11)), TRY(value(15)));
        break;
    case CSS::TransformFunction::Translate:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, 0, 0, TRY(value(0, width)),
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        if (count == 2)
            return Gfx::FloatMatrix4x4(1, 0, 0, TRY(value(0, width)),
                0, 1, 0, TRY(value(1, height)),
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::Translate3d:
        return Gfx::FloatMatrix4x4(1, 0, 0, TRY(value(0, width)),
            0, 1, 0, TRY(value(1, height)),
            0, 0, 1, TRY(value(2)),
            0, 0, 0, 1);
        break;
    case CSS::TransformFunction::TranslateX:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, 0, 0, TRY(value(0, width)),
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::TranslateY:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, TRY(value(0, height)),
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::TranslateZ:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, TRY(value(0)),
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::Scale:
        if (count == 1)
            return Gfx::FloatMatrix4x4(TRY(value(0)), 0, 0, 0,
                0, TRY(value(0)), 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        if (count == 2)
            return Gfx::FloatMatrix4x4(TRY(value(0)), 0, 0, 0,
                0, TRY(value(1)), 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::Scale3d:
        if (count == 3)
            return Gfx::FloatMatrix4x4(TRY(value(0)), 0, 0, 0,
                0, TRY(value(1)), 0, 0,
                0, 0, TRY(value(2)), 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::ScaleX:
        if (count == 1)
            return Gfx::FloatMatrix4x4(TRY(value(0)), 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::ScaleY:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, 0, 0, 0,
                0, TRY(value(0)), 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::ScaleZ:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, TRY(value(0)), 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::Rotate3d:
        if (count == 4)
            return Gfx::rotation_matrix({ TRY(value(0)), TRY(value(1)), TRY(value(2)) }, TRY(value(3)));
        break;
    case CSS::TransformFunction::RotateX:
        if (count == 1)
            return Gfx::rotation_matrix({ 1.0f, 0.0f, 0.0f }, TRY(value(0)));
        break;
    case CSS::TransformFunction::RotateY:
        if (count == 1)
            return Gfx::rotation_matrix({ 0.0f, 1.0f, 0.0f }, TRY(value(0)));
        break;
    case CSS::TransformFunction::Rotate:
    case CSS::TransformFunction::RotateZ:
        if (count == 1)
            return Gfx::rotation_matrix({ 0.0f, 0.0f, 1.0f }, TRY(value(0)));
        break;
    case CSS::TransformFunction::Skew:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, tanf(TRY(value(0))), 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        if (count == 2)
            return Gfx::FloatMatrix4x4(1, tanf(TRY(value(0))), 0, 0,
                tanf(TRY(value(1))), 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::SkewX:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, tanf(TRY(value(0))), 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    case CSS::TransformFunction::SkewY:
        if (count == 1)
            return Gfx::FloatMatrix4x4(1, 0, 0, 0,
                tanf(TRY(value(0))), 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1);
        break;
    }
    dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Unhandled transformation function {} with {} arguments", to_string(m_function), m_values.size());
    return Gfx::FloatMatrix4x4::identity();
}

}
