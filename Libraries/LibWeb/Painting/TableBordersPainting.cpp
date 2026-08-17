/*
 * Copyright (c) 2023, the SerenityOS developers.
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BordersData.h>
#include <LibWeb/Painting/CollapsedTableBorders.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/TableBordersPainting.h>

namespace Web::Painting {

enum class EdgeDirection {
    Horizontal,
    Vertical,
};

struct DeviceEdge {
    BorderDataDevicePixels data;
    u32 source_order { 0 };

    bool paints() const { return data.width > 0 && data.line_style != CSS::LineStyle::None && data.line_style != CSS::LineStyle::Hidden; }
};

static int edge_style_score(CSS::LineStyle style)
{
    switch (style) {
    case CSS::LineStyle::Inset:
        return 1;
    case CSS::LineStyle::Groove:
        return 2;
    case CSS::LineStyle::Outset:
        return 3;
    case CSS::LineStyle::Ridge:
        return 4;
    case CSS::LineStyle::Dotted:
        return 5;
    case CSS::LineStyle::Dashed:
        return 6;
    case CSS::LineStyle::Solid:
        return 7;
    case CSS::LineStyle::Double:
        return 8;
    default:
        return 0;
    }
}

static bool beats_at_joint(DeviceEdge const& a, DeviceEdge const& b)
{
    if (a.paints() != b.paints())
        return a.paints();
    if (!a.paints())
        return false;
    if (a.data.width != b.data.width)
        return a.data.width > b.data.width;
    auto a_score = edge_style_score(a.data.line_style);
    auto b_score = edge_style_score(b.data.line_style);
    if (a_score != b_score)
        return a_score > b_score;
    return a.source_order < b.source_order;
}

struct JointOutcome {
    bool survives { false };
    DevicePixels crossing_size { 0 };
};

static JointOutcome resolve_joint(DeviceEdge const& self, DeviceEdge const& collinear, DeviceEdge const& perpendicular_a, DeviceEdge const& perpendicular_b)
{
    bool survives = !beats_at_joint(collinear, self)
        && !beats_at_joint(perpendicular_a, self)
        && !beats_at_joint(perpendicular_b, self);
    auto const& crossing = beats_at_joint(perpendicular_b, perpendicular_a) ? perpendicular_b : perpendicular_a;
    return { survives, crossing.paints() ? crossing.data.width : DevicePixels(0) };
}

static DevicePixels joint_start_coordinate(DevicePixels line, JointOutcome const& joint)
{
    auto half = joint.crossing_size.value() / 2;
    if (joint.survives)
        return line - half;
    return line + (joint.crossing_size.value() - half);
}

static DevicePixels joint_end_coordinate(DevicePixels line, JointOutcome const& joint)
{
    auto half = joint.crossing_size.value() / 2;
    if (joint.survives)
        return line + (joint.crossing_size.value() - half);
    return line - half;
}

static void paint_edge(DisplayListRecordingContext& context, DevicePixelRect const& rect, DeviceEdge const& edge, EdgeDirection direction)
{
    if (edge.data.line_style == CSS::LineStyle::Dotted || edge.data.line_style == CSS::LineStyle::Dashed) {
        auto p1 = rect.top_left();
        auto p2 = direction == EdgeDirection::Horizontal ? rect.top_right() : rect.bottom_left();
        auto line_style = edge.data.line_style == CSS::LineStyle::Dotted ? Gfx::LineStyle::Dotted : Gfx::LineStyle::Dashed;
        context.display_list_recorder().draw_line(p1.to_type<int>(), p2.to_type<int>(), edge.data.color, edge.data.width.value(), line_style);
        return;
    }
    // FIXME: Support the remaining line styles instead of rendering them as solid.
    context.display_list_recorder().fill_rect(Gfx::IntRect(rect.location(), rect.size()), edge.data.color);
}

void paint_table_borders(DisplayListRecordingContext& context, Paintable const& table_paintable)
{
    // Painting according to the collapsing border model:
    // https://www.w3.org/TR/CSS22/tables.html#collapsing-borders
    auto const* borders = table_paintable.collapsed_table_borders();
    if (!borders)
        return;
    auto const rows = borders->row_count();
    auto const columns = borders->column_count();
    if (rows == 0 || columns == 0)
        return;

    auto origin = table_paintable.absolute_rect().location();
    Vector<DevicePixels> xs;
    xs.ensure_capacity(columns + 1);
    for (auto offset : borders->column_offsets)
        xs.unchecked_append(context.rounded_device_pixels(origin.x() + offset));
    Vector<DevicePixels> ys;
    ys.ensure_capacity(rows + 1);
    for (auto offset : borders->row_offsets)
        ys.unchecked_append(context.rounded_device_pixels(origin.y() + offset));

    auto to_device_edge = [&](CollapsedBorderEdge const& edge) {
        return DeviceEdge {
            .data = {
                .color = edge.border.color,
                .line_style = edge.border.line_style,
                .width = context.rounded_device_pixels(edge.border.width),
            },
            .source_order = edge.source_order,
        };
    };
    Vector<DeviceEdge> horizontal_edges;
    horizontal_edges.ensure_capacity(borders->horizontal_edges.size());
    for (auto const& edge : borders->horizontal_edges)
        horizontal_edges.unchecked_append(to_device_edge(edge));
    Vector<DeviceEdge> vertical_edges;
    vertical_edges.ensure_capacity(borders->vertical_edges.size());
    for (auto const& edge : borders->vertical_edges)
        vertical_edges.unchecked_append(to_device_edge(edge));

    static DeviceEdge const no_edge {};
    auto horizontal = [&](size_t line, size_t column) -> DeviceEdge const& { return horizontal_edges[line * columns + column]; };
    auto vertical = [&](size_t line, size_t row) -> DeviceEdge const& { return vertical_edges[line * rows + row]; };

    for (size_t i = 0; i <= rows; ++i) {
        for (size_t j = 0; j < columns; ++j) {
            auto const& self = horizontal(i, j);
            if (!self.paints())
                continue;
            auto start_joint = resolve_joint(self,
                j > 0 ? horizontal(i, j - 1) : no_edge,
                i > 0 ? vertical(j, i - 1) : no_edge,
                i < rows ? vertical(j, i) : no_edge);
            auto end_joint = resolve_joint(self,
                j + 1 < columns ? horizontal(i, j + 1) : no_edge,
                i > 0 ? vertical(j + 1, i - 1) : no_edge,
                i < rows ? vertical(j + 1, i) : no_edge);
            auto x0 = joint_start_coordinate(xs[j], start_joint);
            auto x1 = joint_end_coordinate(xs[j + 1], end_joint);
            if (x1 <= x0)
                continue;
            DevicePixelRect rect { x0, ys[i] - self.data.width.value() / 2, x1 - x0, self.data.width };
            paint_edge(context, rect, self, EdgeDirection::Horizontal);
        }
    }
    for (size_t j = 0; j <= columns; ++j) {
        for (size_t i = 0; i < rows; ++i) {
            auto const& self = vertical(j, i);
            if (!self.paints())
                continue;
            auto start_joint = resolve_joint(self,
                i > 0 ? vertical(j, i - 1) : no_edge,
                j > 0 ? horizontal(i, j - 1) : no_edge,
                j < columns ? horizontal(i, j) : no_edge);
            auto end_joint = resolve_joint(self,
                i + 1 < rows ? vertical(j, i + 1) : no_edge,
                j > 0 ? horizontal(i + 1, j - 1) : no_edge,
                j < columns ? horizontal(i + 1, j) : no_edge);
            auto y0 = joint_start_coordinate(ys[i], start_joint);
            auto y1 = joint_end_coordinate(ys[i + 1], end_joint);
            if (y1 <= y0)
                continue;
            DevicePixelRect rect { xs[j] - self.data.width.value() / 2, y0, self.data.width, y1 - y0 };
            paint_edge(context, rect, self, EdgeDirection::Vertical);
        }
    }
}

}
