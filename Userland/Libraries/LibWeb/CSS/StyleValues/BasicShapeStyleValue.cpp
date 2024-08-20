/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BasicShapeStyleValue.h"
#include <LibGfx/Path.h>

namespace Web::CSS {

Gfx::Path Polygon::to_path(CSSPixelRect reference_box, Layout::Node const& node) const
{
    Gfx::Path path;
    bool first = true;
    for (auto const& point : points) {
        Gfx::FloatPoint resolved_point {
            static_cast<float>(point.x.to_px(node, reference_box.width())),
            static_cast<float>(point.y.to_px(node, reference_box.height()))
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
