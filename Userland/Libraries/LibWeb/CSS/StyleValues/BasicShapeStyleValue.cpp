/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BasicShapeStyleValue.h"
#include <LibGfx/Path.h>

namespace Web::CSS {

static Gfx::Path path_from_resolved_rect(float top, float right, float bottom, float left)
{
    Gfx::Path path;
    path.move_to(Gfx::FloatPoint { left, top });
    path.line_to(Gfx::FloatPoint { right, top });
    path.line_to(Gfx::FloatPoint { right, bottom });
    path.line_to(Gfx::FloatPoint { left, bottom });
    path.close();
    return path;
}

Gfx::Path Inset::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    // FIXME: A pair of insets in either dimension that add up to more than the used dimension
    // (such as left and right insets of 75% apiece) use the CSS Backgrounds 3 § 4.5 Overlapping Curves rules
    // to proportionally reduce the inset effect to 100%.

    auto top = inset_box.top().to_px(node, reference_box.height()).to_float();
    auto right = reference_box.width().to_float() - inset_box.right().to_px(node, reference_box.width()).to_float();
    auto bottom = reference_box.height().to_float() - inset_box.bottom().to_px(node, reference_box.height()).to_float();
    auto left = inset_box.left().to_px(node, reference_box.width()).to_float();

    return path_from_resolved_rect(top, right, bottom, left);
}

String Inset::to_string() const
{
    return MUST(String::formatted("inset({} {} {} {})", inset_box.top(), inset_box.right(), inset_box.bottom(), inset_box.left()));
}

Gfx::Path Xywh::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    auto top = y.to_px(node, reference_box.height()).to_float();
    auto bottom = top + max(0.0f, height.to_px(node, reference_box.height()).to_float());
    auto left = x.to_px(node, reference_box.width()).to_float();
    auto right = left + max(0.0f, width.to_px(node, reference_box.width()).to_float());

    return path_from_resolved_rect(top, right, bottom, left);
}

String Xywh::to_string() const
{
    return MUST(String::formatted("xywh({} {} {} {})", x, y, width, height));
}

Gfx::Path Rect::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    // An auto value makes the edge of the box coincide with the corresponding edge of the reference box:
    // it’s equivalent to 0% as the first (top) or fourth (left) value, and equivalent to 100% as the second (right) or third (bottom) value.

    auto top = box.top().is_auto() ? 0 : box.top().to_px(node, reference_box.height()).to_float();
    auto right = box.right().is_auto() ? reference_box.width().to_float() : box.right().to_px(node, reference_box.width()).to_float();
    auto bottom = box.bottom().is_auto() ? reference_box.height().to_float() : box.bottom().to_px(node, reference_box.height()).to_float();
    auto left = box.left().is_auto() ? 0 : box.left().to_px(node, reference_box.width()).to_float();

    // The second (right) and third (bottom) values are floored by the fourth (left) and second (top) values, respectively.
    return path_from_resolved_rect(top, max(right, left), max(bottom, top), left);
}

String Rect::to_string() const
{
    return MUST(String::formatted("rect({} {} {} {})", box.top(), box.right(), box.bottom(), box.left()));
}

static String radius_to_string(ShapeRadius radius)
{
    return radius.visit(
        [](LengthPercentage const& length_percentage) { return length_percentage.to_string(); },
        [](FitSide const& side) {
            switch (side) {
            case FitSide::ClosestSide:
                return "closest-side"_string;
            case FitSide::FarthestSide:
                return "farthest-side"_string;
            }
            VERIFY_NOT_REACHED();
        });
}

Gfx::Path Circle::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    // Translating the reference box because PositionStyleValues are resolved to an absolute position.
    auto center = position->resolved(node, reference_box.translated(-reference_box.x(), -reference_box.y()));

    float radius_px = radius.visit(
        [&](LengthPercentage const& length_percentage) {
            auto radius_ref = sqrt(pow(reference_box.width().to_float(), 2) + pow(reference_box.height().to_float(), 2)) / AK::Sqrt2<float>;
            return max(0.0f, length_percentage.to_px(node, CSSPixels(radius_ref)).to_float());
        },
        [&](FitSide const& side) {
            switch (side) {
            case FitSide::ClosestSide:
                float closest;
                closest = min(abs(center.x()), abs(center.y())).to_float();
                closest = min(closest, abs(reference_box.width() - center.x()).to_float());
                closest = min(closest, abs(reference_box.height() - center.y()).to_float());
                return closest;
            case FitSide::FarthestSide:
                float farthest;
                farthest = max(abs(center.x()), abs(center.y())).to_float();
                farthest = max(farthest, abs(reference_box.width() - center.x()).to_float());
                farthest = max(farthest, abs(reference_box.height() - center.y()).to_float());
                return farthest;
            }
            VERIFY_NOT_REACHED();
        });

    Gfx::Path path;
    path.move_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_px });
    path.arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() - radius_px }, radius_px, true, true);
    path.arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_px }, radius_px, true, true);
    return path;
}

String Circle::to_string() const
{
    return MUST(String::formatted("circle({} at {})", radius_to_string(radius), position->to_string()));
}

Gfx::Path Ellipse::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    // Translating the reference box because PositionStyleValues are resolved to an absolute position.
    auto center = position->resolved(node, reference_box.translated(-reference_box.x(), -reference_box.y()));

    float radius_x_px = radius_x.visit(
        [&](LengthPercentage const& length_percentage) {
            return max(0.0f, length_percentage.to_px(node, reference_box.width()).to_float());
        },
        [&](FitSide const& side) {
            switch (side) {
            case FitSide::ClosestSide:
                return min(abs(center.x()), abs(reference_box.width() - center.x())).to_float();
            case FitSide::FarthestSide:
                return max(abs(center.x()), abs(reference_box.width() - center.x())).to_float();
            }
            VERIFY_NOT_REACHED();
        });

    float radius_y_px = radius_y.visit(
        [&](LengthPercentage const& length_percentage) {
            return max(0.0f, length_percentage.to_px(node, reference_box.height()).to_float());
        },
        [&](FitSide const& side) {
            switch (side) {
            case FitSide::ClosestSide:
                return min(abs(center.y()), abs(reference_box.height() - center.y())).to_float();
            case FitSide::FarthestSide:
                return max(abs(center.y()), abs(reference_box.height() - center.y())).to_float();
            }
            VERIFY_NOT_REACHED();
        });

    Gfx::Path path;
    path.move_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_y_px });
    path.elliptical_arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() - radius_y_px }, Gfx::FloatSize { radius_x_px, radius_y_px }, 0, true, true);
    path.elliptical_arc_to(Gfx::FloatPoint { center.x().to_float(), center.y().to_float() + radius_y_px }, Gfx::FloatSize { radius_x_px, radius_y_px }, 0, true, true);
    return path;
}

String Ellipse::to_string() const
{
    return MUST(String::formatted("ellipse({} {} at {})", radius_to_string(radius_x), radius_to_string(radius_y), position->to_string()));
}

Gfx::Path Polygon::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    Gfx::Path path;
    bool first = true;
    for (auto const& point : points) {
        Gfx::FloatPoint resolved_point {
            point.x.to_px(node, reference_box.width()).to_float(),
            point.y.to_px(node, reference_box.height()).to_float()
        };
        if (first)
            path.move_to(resolved_point);
        else
            path.line_to(resolved_point);
        first = false;
    }
    path.close();
    return path;
}

String Polygon::to_string() const
{
    StringBuilder builder;
    builder.append("polygon("sv);
    bool first = true;
    for (auto const& point : points) {
        if (!first)
            builder.append(',');
        builder.appendff("{} {}", point.x, point.y);
        first = false;
    }
    builder.append(')');
    return MUST(builder.to_string());
}

BasicShapeStyleValue::~BasicShapeStyleValue() = default;

Gfx::Path BasicShapeStyleValue::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    return m_basic_shape.visit([&](auto const& shape) {
        return shape.to_path(reference_box, node);
    });
}

String BasicShapeStyleValue::to_string() const
{
    return m_basic_shape.visit([](auto const& shape) {
        return shape.to_string();
    });
}

}
