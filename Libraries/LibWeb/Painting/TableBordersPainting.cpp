/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Traits.h>
#include <LibWeb/Layout/TableFormattingContext.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TableBordersPainting.h>

struct CellCoordinates {
    size_t row_index;
    size_t column_index;

    bool operator==(CellCoordinates const& other) const
    {
        return row_index == other.row_index && column_index == other.column_index;
    }
};

namespace AK {
template<>
struct Traits<CellCoordinates> : public DefaultTraits<CellCoordinates> {
    static unsigned hash(CellCoordinates const& key) { return pair_int_hash(key.row_index, key.column_index); }
};
}

namespace Web::Painting {

static void collect_cell_boxes(Vector<PaintableBox const&>& cell_boxes, PaintableBox const& table_paintable)
{
    table_paintable.for_each_child_of_type<PaintableBox>([&](auto& child) {
        if (child.display().is_table_cell()) {
            cell_boxes.append(child);
        } else {
            collect_cell_boxes(cell_boxes, child);
        }
        return IterationDecision::Continue;
    });
}

enum class EdgeDirection {
    Horizontal,
    Vertical,
};

struct DeviceBorderData {
    Color color { Color::Transparent };
    CSS::LineStyle line_style { CSS::LineStyle::None };
    DevicePixels width { 0 };
};

struct DeviceBorderDataWithElementKind {
    DeviceBorderData border_data;
    Painting::PaintableBox::ConflictingElementKind element_kind { Painting::PaintableBox::ConflictingElementKind::Cell };
};

struct DeviceBordersDataWithElementKind {
    DeviceBorderDataWithElementKind top;
    DeviceBorderDataWithElementKind right;
    DeviceBorderDataWithElementKind bottom;
    DeviceBorderDataWithElementKind left;
};

struct BorderEdgePaintingInfo {
    DevicePixelRect rect;
    DeviceBorderDataWithElementKind border_data_with_element_kind;
    EdgeDirection direction;
    Optional<size_t> row;
    Optional<size_t> column;
};

static Optional<size_t> row_index_for_element_kind(size_t index, Painting::PaintableBox::ConflictingElementKind element_kind)
{
    switch (element_kind) {
    case Painting::PaintableBox::ConflictingElementKind::Cell:
    case Painting::PaintableBox::ConflictingElementKind::Row:
    case Painting::PaintableBox::ConflictingElementKind::RowGroup: {
        return index;
    }
    default:
        return {};
    }
}

static Optional<size_t> column_index_for_element_kind(size_t index, Painting::PaintableBox::ConflictingElementKind element_kind)
{
    switch (element_kind) {
    case Painting::PaintableBox::ConflictingElementKind::Cell:
    case Painting::PaintableBox::ConflictingElementKind::Column:
    case Painting::PaintableBox::ConflictingElementKind::ColumnGroup: {
        return index;
    }
    default:
        return {};
    }
}

static DevicePixels half_ceil(DevicePixels width)
{
    return ceil(static_cast<double>(width.value()) / 2);
}

static DevicePixels half_floor(DevicePixels width)
{
    return floor(static_cast<double>(width.value()) / 2);
}

static BorderEdgePaintingInfo make_right_cell_edge(
    DevicePixelRect const& right_cell_rect,
    DevicePixelRect const& cell_rect,
    DeviceBordersDataWithElementKind const& borders_data,
    CellCoordinates const& coordinates)
{
    auto connect_top_offset = half_ceil(borders_data.top.border_data.width);
    auto connect_excess_height = connect_top_offset + half_floor(borders_data.bottom.border_data.width);
    DevicePixelRect right_border_rect = {
        right_cell_rect.x() - half_ceil(borders_data.right.border_data.width),
        cell_rect.y() - connect_top_offset,
        borders_data.right.border_data.width,
        max(cell_rect.height(), right_cell_rect.height()) + connect_excess_height,
    };
    return BorderEdgePaintingInfo {
        .rect = right_border_rect,
        .border_data_with_element_kind = borders_data.right,
        .direction = EdgeDirection::Vertical,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.right.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.right.element_kind),
    };
}

static BorderEdgePaintingInfo make_down_cell_edge(
    DevicePixelRect const& down_cell_rect,
    DevicePixelRect const& cell_rect,
    DeviceBordersDataWithElementKind const& borders_data,
    CellCoordinates const& coordinates)
{
    auto connect_left_offset = half_ceil(borders_data.left.border_data.width);
    auto connect_excess_width = connect_left_offset + half_floor(borders_data.right.border_data.width);
    DevicePixelRect down_border_rect = {
        cell_rect.x() - connect_left_offset,
        down_cell_rect.y() - half_ceil(borders_data.bottom.border_data.width),
        max(cell_rect.width(), down_cell_rect.width()) + connect_excess_width,
        borders_data.bottom.border_data.width,
    };
    return BorderEdgePaintingInfo {
        .rect = down_border_rect,
        .border_data_with_element_kind = borders_data.bottom,
        .direction = EdgeDirection::Horizontal,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.bottom.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.bottom.element_kind),
    };
}

static BorderEdgePaintingInfo make_first_row_top_cell_edge(DevicePixelRect const& cell_rect, DeviceBordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    auto connect_left_offset = half_ceil(borders_data.left.border_data.width.value());
    auto connect_excess_width = connect_left_offset + half_floor(borders_data.right.border_data.width.value());
    DevicePixelRect top_border_rect = {
        cell_rect.x() - connect_left_offset,
        cell_rect.y() - half_ceil(borders_data.top.border_data.width.value()),
        cell_rect.width() + connect_excess_width,
        borders_data.top.border_data.width,
    };
    return BorderEdgePaintingInfo {
        .rect = top_border_rect,
        .border_data_with_element_kind = borders_data.top,
        .direction = EdgeDirection::Horizontal,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.top.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.top.element_kind),
    };
}

static BorderEdgePaintingInfo make_last_row_bottom_cell_edge(DevicePixelRect const& cell_rect, DeviceBordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    auto connect_left_offset = half_ceil(borders_data.left.border_data.width);
    auto connect_excess_width = connect_left_offset + half_floor(borders_data.right.border_data.width);
    DevicePixelRect bottom_border_rect = {
        cell_rect.x() - connect_left_offset,
        cell_rect.y() + cell_rect.height() - half_ceil(borders_data.bottom.border_data.width),
        cell_rect.width() + connect_excess_width,
        borders_data.bottom.border_data.width,
    };
    return BorderEdgePaintingInfo {
        .rect = bottom_border_rect,
        .border_data_with_element_kind = borders_data.bottom,
        .direction = EdgeDirection::Horizontal,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.bottom.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.bottom.element_kind),
    };
}

static BorderEdgePaintingInfo make_first_column_left_cell_edge(DevicePixelRect const& cell_rect, DeviceBordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    auto connect_top_offset = half_ceil(borders_data.top.border_data.width);
    auto connect_excess_height = connect_top_offset + half_floor(borders_data.bottom.border_data.width);
    DevicePixelRect left_border_rect = {
        cell_rect.x() - half_ceil(borders_data.left.border_data.width),
        cell_rect.y() - connect_top_offset,
        borders_data.left.border_data.width,
        cell_rect.height() + connect_excess_height,
    };
    return BorderEdgePaintingInfo {
        .rect = left_border_rect,
        .border_data_with_element_kind = borders_data.left,
        .direction = EdgeDirection::Vertical,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.left.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.left.element_kind),
    };
}

static BorderEdgePaintingInfo make_last_column_right_cell_edge(DevicePixelRect const& cell_rect, DeviceBordersDataWithElementKind const& borders_data, CellCoordinates const& coordinates)
{
    auto connect_top_offset = half_ceil(borders_data.top.border_data.width);
    auto connect_excess_height = connect_top_offset + half_floor(borders_data.bottom.border_data.width);
    DevicePixelRect right_border_rect = {
        cell_rect.x() + cell_rect.width() - half_ceil(borders_data.right.border_data.width),
        cell_rect.y() - connect_top_offset,
        borders_data.right.border_data.width,
        cell_rect.height() + connect_excess_height,
    };
    return BorderEdgePaintingInfo {
        .rect = right_border_rect,
        .border_data_with_element_kind = borders_data.right,
        .direction = EdgeDirection::Vertical,
        .row = row_index_for_element_kind(coordinates.row_index, borders_data.right.element_kind),
        .column = column_index_for_element_kind(coordinates.column_index, borders_data.right.element_kind),
    };
}

static CSS::BorderData css_border_data_from_device_border_data(DeviceBorderData const& device_border_data)
{
    return CSS::BorderData {
        .color = device_border_data.color,
        .line_style = device_border_data.line_style,
        .width = device_border_data.width.value(),
    };
}

static void paint_collected_edges(PaintContext& context, Vector<BorderEdgePaintingInfo>& border_edge_painting_info_list)
{
    // This sorting step isn't part of the specification, but it matches the behavior of other browsers at border intersections, which aren't
    // part of border conflict resolution in the specification but it's still desirable to handle them in a way which is consistent with it.
    // See https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution for reference.
    quick_sort(border_edge_painting_info_list, [](auto const& a, auto const& b) {
        auto const& a_border_data = a.border_data_with_element_kind.border_data;
        auto const& b_border_data = b.border_data_with_element_kind.border_data;
        if (a_border_data.line_style == b_border_data.line_style && a_border_data.width == b_border_data.width) {
            if (b.border_data_with_element_kind.element_kind < a.border_data_with_element_kind.element_kind) {
                return true;
            } else if (b.border_data_with_element_kind.element_kind > a.border_data_with_element_kind.element_kind) {
                return false;
            }
            // Here the element kind is the same, thus the coordinates are either both set or not set.
            VERIFY(a.column.has_value() == b.column.has_value());
            VERIFY(a.row.has_value() == b.row.has_value());
            if (a.column.has_value()) {
                if (b.column.value() < a.column.value()) {
                    return true;
                } else if (b.column.value() > a.column.value()) {
                    return false;
                }
            }
            return a.row.has_value() ? b.row.value() < a.row.value() : false;
        }
        return Layout::TableFormattingContext::border_is_less_specific(
            css_border_data_from_device_border_data(a_border_data),
            css_border_data_from_device_border_data(b_border_data));
    });

    for (auto const& border_edge_painting_info : border_edge_painting_info_list) {
        auto const& border_data_with_element_kind = border_edge_painting_info.border_data_with_element_kind;
        auto width = border_data_with_element_kind.border_data.width;
        if (width <= 0)
            continue;
        auto color = border_data_with_element_kind.border_data.color;
        auto border_style = border_data_with_element_kind.border_data.line_style;
        auto p1 = border_edge_painting_info.rect.top_left();
        auto p2 = border_edge_painting_info.direction == EdgeDirection::Horizontal
            ? border_edge_painting_info.rect.top_right()
            : border_edge_painting_info.rect.bottom_left();

        if (border_style == CSS::LineStyle::Dotted) {
            context.display_list_recorder().draw_line(p1.to_type<int>(), p2.to_type<int>(), color, width.value(), Gfx::LineStyle::Dotted);
        } else if (border_style == CSS::LineStyle::Dashed) {
            context.display_list_recorder().draw_line(p1.to_type<int>(), p2.to_type<int>(), color, width.value(), Gfx::LineStyle::Dashed);
        } else {
            // FIXME: Support the remaining line styles instead of rendering them as solid.
            context.display_list_recorder().fill_rect(Gfx::IntRect(border_edge_painting_info.rect.location(), border_edge_painting_info.rect.size()), color);
        }
    }
}

static HashMap<CellCoordinates, DevicePixelRect> snap_cells_to_device_coordinates(HashMap<CellCoordinates, PaintableBox const*> const& cell_coordinates_to_box, size_t row_count, size_t column_count, PaintContext const& context)
{
    Vector<DevicePixels> y_line_start_coordinates;
    Vector<DevicePixels> y_line_end_coordinates;
    y_line_start_coordinates.resize(row_count + 1);
    y_line_end_coordinates.resize(row_count + 1);
    Vector<DevicePixels> x_line_start_coordinates;
    Vector<DevicePixels> x_line_end_coordinates;
    x_line_start_coordinates.resize(column_count + 1);
    x_line_end_coordinates.resize(column_count + 1);
    for (auto const& kv : cell_coordinates_to_box) {
        auto const& cell_box = kv.value;
        auto start_row_index = cell_box->table_cell_coordinates()->row_index;
        auto end_row_index = start_row_index + cell_box->table_cell_coordinates()->row_span;
        auto cell_rect = cell_box->absolute_border_box_rect();
        y_line_start_coordinates[start_row_index] = max(context.rounded_device_pixels(cell_rect.y()), y_line_start_coordinates[start_row_index]);
        y_line_end_coordinates[end_row_index] = max(context.rounded_device_pixels(cell_rect.y() + cell_rect.height()), y_line_end_coordinates[end_row_index]);
        auto start_column_index = cell_box->table_cell_coordinates()->column_index;
        auto end_column_index = start_column_index + cell_box->table_cell_coordinates()->column_span;
        x_line_start_coordinates[start_column_index] = max(context.rounded_device_pixels(cell_rect.x()), x_line_start_coordinates[start_column_index]);
        x_line_end_coordinates[end_column_index] = max(context.rounded_device_pixels(cell_rect.x() + cell_rect.width()), x_line_end_coordinates[end_column_index]);
    }
    HashMap<CellCoordinates, DevicePixelRect> cell_coordinates_to_device_rect;
    for (auto const& kv : cell_coordinates_to_box) {
        auto const& cell_box = kv.value;
        auto start_row_index = cell_box->table_cell_coordinates()->row_index;
        auto end_row_index = start_row_index + cell_box->table_cell_coordinates()->row_span;
        auto height = y_line_end_coordinates[end_row_index] - y_line_start_coordinates[start_row_index];
        auto start_column_index = cell_box->table_cell_coordinates()->column_index;
        auto end_column_index = start_column_index + cell_box->table_cell_coordinates()->column_span;
        auto width = x_line_end_coordinates[end_column_index] - x_line_start_coordinates[start_column_index];
        cell_coordinates_to_device_rect.set(kv.key, DevicePixelRect { x_line_start_coordinates[start_column_index], y_line_start_coordinates[start_row_index], width, height });
    }
    return cell_coordinates_to_device_rect;
}

static DeviceBorderDataWithElementKind device_border_data_from_css_border_data(Painting::PaintableBox::BorderDataWithElementKind const& border_data_with_element_kind, PaintContext const& context)
{
    return DeviceBorderDataWithElementKind {
        .border_data = {
            .color = border_data_with_element_kind.border_data.color,
            .line_style = border_data_with_element_kind.border_data.line_style,
            .width = context.rounded_device_pixels(border_data_with_element_kind.border_data.width),
        },
        .element_kind = border_data_with_element_kind.element_kind,
    };
}

static void paint_separate_cell_borders(PaintableBox const& cell_box, HashMap<CellCoordinates, DevicePixelRect> const& cell_coordinates_to_device_rect, PaintContext& context)
{
    auto borders_data = cell_box.override_borders_data().has_value() ? PaintableBox::remove_element_kind_from_borders_data(cell_box.override_borders_data().value()) : BordersData {
        .top = cell_box.box_model().border.top == 0 ? CSS::BorderData() : cell_box.computed_values().border_top(),
        .right = cell_box.box_model().border.right == 0 ? CSS::BorderData() : cell_box.computed_values().border_right(),
        .bottom = cell_box.box_model().border.bottom == 0 ? CSS::BorderData() : cell_box.computed_values().border_bottom(),
        .left = cell_box.box_model().border.left == 0 ? CSS::BorderData() : cell_box.computed_values().border_left(),
    };
    auto cell_rect = cell_coordinates_to_device_rect.get({ cell_box.table_cell_coordinates()->row_index, cell_box.table_cell_coordinates()->column_index }).value();
    paint_all_borders(context.display_list_recorder(), cell_rect, cell_box.normalized_border_radii_data().as_corners(context), borders_data.to_device_pixels(context));
}

void paint_table_borders(PaintContext& context, PaintableBox const& table_paintable)
{
    // Partial implementation of painting according to the collapsing border model:
    // https://www.w3.org/TR/CSS22/tables.html#collapsing-borders
    Vector<PaintableBox const&> cell_boxes;
    collect_cell_boxes(cell_boxes, table_paintable);
    Vector<BorderEdgePaintingInfo> border_edge_painting_info_list;
    HashMap<CellCoordinates, PaintableBox const*> cell_coordinates_to_box;
    size_t row_count = 0;
    size_t column_count = 0;
    for (auto const& cell_box : cell_boxes) {
        cell_coordinates_to_box.set(CellCoordinates {
                                        .row_index = cell_box.table_cell_coordinates()->row_index,
                                        .column_index = cell_box.table_cell_coordinates()->column_index },
            &cell_box);
        row_count = max(row_count, cell_box.table_cell_coordinates()->row_index + cell_box.table_cell_coordinates()->row_span);
        column_count = max(column_count, cell_box.table_cell_coordinates()->column_index + cell_box.table_cell_coordinates()->column_span);
    }
    auto cell_coordinates_to_device_rect = snap_cells_to_device_coordinates(cell_coordinates_to_box, row_count, column_count, context);
    for (auto const& cell_box : cell_boxes) {
        if (cell_box.computed_values().border_collapse() == CSS::BorderCollapse::Separate) {
            paint_separate_cell_borders(cell_box, cell_coordinates_to_device_rect, context);
            continue;
        }
        auto css_borders_data = cell_box.override_borders_data().has_value() ? cell_box.override_borders_data().value() : PaintableBox::BordersDataWithElementKind {
            .top = { .border_data = cell_box.box_model().border.top == 0 ? CSS::BorderData() : cell_box.computed_values().border_top(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
            .right = { .border_data = cell_box.box_model().border.right == 0 ? CSS::BorderData() : cell_box.computed_values().border_right(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
            .bottom = { .border_data = cell_box.box_model().border.bottom == 0 ? CSS::BorderData() : cell_box.computed_values().border_bottom(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
            .left = { .border_data = cell_box.box_model().border.left == 0 ? CSS::BorderData() : cell_box.computed_values().border_left(), .element_kind = PaintableBox::ConflictingElementKind::Cell },
        };
        DeviceBordersDataWithElementKind borders_data = {
            .top = device_border_data_from_css_border_data(css_borders_data.top, context),
            .right = device_border_data_from_css_border_data(css_borders_data.right, context),
            .bottom = device_border_data_from_css_border_data(css_borders_data.bottom, context),
            .left = device_border_data_from_css_border_data(css_borders_data.left, context),
        };
        auto cell_rect = cell_coordinates_to_device_rect.get({ cell_box.table_cell_coordinates()->row_index, cell_box.table_cell_coordinates()->column_index }).value();
        CellCoordinates right_cell_coordinates {
            .row_index = cell_box.table_cell_coordinates()->row_index,
            .column_index = cell_box.table_cell_coordinates()->column_index + cell_box.table_cell_coordinates()->column_span
        };
        auto maybe_right_cell = cell_coordinates_to_device_rect.get(right_cell_coordinates);
        CellCoordinates down_cell_coordinates {
            .row_index = cell_box.table_cell_coordinates()->row_index + cell_box.table_cell_coordinates()->row_span,
            .column_index = cell_box.table_cell_coordinates()->column_index
        };
        auto maybe_down_cell = cell_coordinates_to_device_rect.get(down_cell_coordinates);
        if (maybe_right_cell.has_value())
            border_edge_painting_info_list.append(make_right_cell_edge(maybe_right_cell.value(), cell_rect, borders_data, right_cell_coordinates));
        if (maybe_down_cell.has_value())
            border_edge_painting_info_list.append(make_down_cell_edge(maybe_down_cell.value(), cell_rect, borders_data, down_cell_coordinates));
        if (cell_box.table_cell_coordinates()->row_index == 0)
            border_edge_painting_info_list.append(make_first_row_top_cell_edge(cell_rect, borders_data,
                { .row_index = 0, .column_index = cell_box.table_cell_coordinates()->column_index }));
        if (cell_box.table_cell_coordinates()->row_index + cell_box.table_cell_coordinates()->row_span == row_count)
            border_edge_painting_info_list.append(make_last_row_bottom_cell_edge(cell_rect, borders_data,
                { .row_index = row_count - 1, .column_index = cell_box.table_cell_coordinates()->column_index }));
        if (cell_box.table_cell_coordinates()->column_index == 0)
            border_edge_painting_info_list.append(make_first_column_left_cell_edge(cell_rect, borders_data,
                { .row_index = cell_box.table_cell_coordinates()->row_index, .column_index = 0 }));
        if (cell_box.table_cell_coordinates()->column_index + cell_box.table_cell_coordinates()->column_span == column_count)
            border_edge_painting_info_list.append(make_last_column_right_cell_edge(cell_rect, borders_data,
                { .row_index = cell_box.table_cell_coordinates()->row_index, .column_index = column_count - 1 }));
    }

    paint_collected_edges(context, border_edge_painting_info_list);

    for (auto const& cell_box : cell_boxes) {
        auto const& border_radii_data = cell_box.normalized_border_radii_data();
        auto top_left = border_radii_data.top_left.as_corner(context);
        auto top_right = border_radii_data.top_right.as_corner(context);
        auto bottom_right = border_radii_data.bottom_right.as_corner(context);
        auto bottom_left = border_radii_data.bottom_left.as_corner(context);
        if (!top_left && !top_right && !bottom_left && !bottom_right) {
            continue;
        } else {
            auto borders_data = cell_box.override_borders_data().has_value() ? PaintableBox::remove_element_kind_from_borders_data(cell_box.override_borders_data().value()) : BordersData {
                .top = cell_box.box_model().border.top == 0 ? CSS::BorderData() : cell_box.computed_values().border_top(),
                .right = cell_box.box_model().border.right == 0 ? CSS::BorderData() : cell_box.computed_values().border_right(),
                .bottom = cell_box.box_model().border.bottom == 0 ? CSS::BorderData() : cell_box.computed_values().border_bottom(),
                .left = cell_box.box_model().border.left == 0 ? CSS::BorderData() : cell_box.computed_values().border_left(),
            };
            paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(cell_box.absolute_border_box_rect()), cell_box.normalized_border_radii_data().as_corners(context), borders_data.to_device_pixels(context));
        }
    }
}
}
