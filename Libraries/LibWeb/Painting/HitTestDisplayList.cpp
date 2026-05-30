/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <math.h>

namespace Web::Painting {

static constexpr double spatial_index_cell_size = 128.0;
static constexpr size_t max_bucketed_cells_per_item = 64;
// Treat small block-axis gaps between caret line fragments as the same visual row.
static constexpr CSSPixels caret_line_block_axis_range_slop = 4;
static constexpr CSSPixels caret_item_block_axis_compare_slop = 32;

static i32 spatial_index_cell_for(CSSPixels offset)
{
    return static_cast<i32>(floor(offset.to_double() / spatial_index_cell_size));
}

static u64 spatial_index_cell_key(i32 x, i32 y)
{
    return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(y);
}

static bool writing_mode_is_horizontal(CSS::WritingMode writing_mode)
{
    return writing_mode == CSS::WritingMode::HorizontalTb;
}

static CSSPixels block_axis_start(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.top() : rect.left();
}

static CSSPixels block_axis_end(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.bottom() : rect.right();
}

static CSSPixels block_axis_coordinate(CSSPixelPoint point, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? point.y() : point.x();
}

static CSSPixels inline_axis_start(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.left() : rect.top();
}

static CSSPixels inline_axis_end(CSSPixelRect rect, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? rect.right() : rect.bottom();
}

static CSSPixels inline_axis_coordinate(CSSPixelPoint point, CSS::WritingMode writing_mode)
{
    return writing_mode_is_horizontal(writing_mode) ? point.x() : point.y();
}

static bool rects_overlap_in_block_axis(CSSPixelRect a, CSSPixelRect b, CSS::WritingMode writing_mode)
{
    return block_axis_start(a, writing_mode) < block_axis_end(b, writing_mode)
        && block_axis_start(b, writing_mode) < block_axis_end(a, writing_mode);
}

static CSSPixels distance_to_range(CSSPixels coordinate, CSSPixels start, CSSPixels end)
{
    if (coordinate < start)
        return start - coordinate;
    if (coordinate > end)
        return coordinate - end;
    return 0;
}

static CSSPixels block_axis_distance_to_rect(CSSPixelRect rect, CSSPixelPoint point, CSS::WritingMode writing_mode)
{
    return distance_to_range(block_axis_coordinate(point, writing_mode), block_axis_start(rect, writing_mode), block_axis_end(rect, writing_mode));
}

static CSSPixels block_axis_distance_to_line_rect(CSSPixelRect rect, CSSPixelPoint point, CSS::WritingMode writing_mode)
{
    return distance_to_range(block_axis_coordinate(point, writing_mode), block_axis_start(rect, writing_mode) - caret_line_block_axis_range_slop, block_axis_end(rect, writing_mode) + caret_line_block_axis_range_slop);
}

static CSSPixels inline_axis_distance_to_rect(CSSPixelRect rect, CSSPixelPoint point, CSS::WritingMode writing_mode)
{
    return distance_to_range(inline_axis_coordinate(point, writing_mode), inline_axis_start(rect, writing_mode), inline_axis_end(rect, writing_mode));
}

static CSSPixels absolute_difference(CSSPixels a, CSSPixels b)
{
    return a > b ? a - b : b - a;
}

static bool caret_line_is_better_candidate(CSSPixels block_distance, CSSPixels inline_distance, CSSPixels closest_block_distance, CSSPixels closest_inline_distance, CSSPixels block_axis_compare_slop)
{
    if (absolute_difference(block_distance, closest_block_distance) <= block_axis_compare_slop) {
        if (inline_distance != closest_inline_distance)
            return inline_distance < closest_inline_distance;
        return block_distance < closest_block_distance;
    }

    return block_distance < closest_block_distance;
}

static bool local_point_is_before_box(Paintable const& paintable, CSSPixelRect rect, CSSPixelPoint local_point)
{
    auto const& computed_values = paintable.computed_values();
    auto writing_mode = computed_values.writing_mode();

    auto block_coordinate = block_axis_coordinate(local_point, writing_mode);
    if (block_coordinate < block_axis_start(rect, writing_mode))
        return !computed_values.block_axis_is_reverse();
    if (block_coordinate >= block_axis_end(rect, writing_mode))
        return computed_values.block_axis_is_reverse();

    auto inline_start = inline_axis_start(rect, writing_mode);
    auto inline_end = inline_axis_end(rect, writing_mode);
    auto inline_middle = inline_start + (inline_end - inline_start).scaled(0.5);
    auto inline_coordinate = inline_axis_coordinate(local_point, writing_mode);
    return computed_values.inline_axis_is_reverse()
        ? inline_coordinate > inline_middle
        : inline_coordinate <= inline_middle;
}

static bool local_point_is_before_line(Paintable const& paintable, CSSPixelRect rect, CSSPixelPoint local_point)
{
    auto const& computed_values = paintable.computed_values();
    auto writing_mode = computed_values.writing_mode();
    auto block_coordinate = block_axis_coordinate(local_point, writing_mode);
    if (block_coordinate < block_axis_start(rect, writing_mode))
        return !computed_values.block_axis_is_reverse();
    if (block_coordinate >= block_axis_end(rect, writing_mode))
        return computed_values.block_axis_is_reverse();
    return false;
}

NonnullRefPtr<HitTestDisplayList> HitTestDisplayList::create(u64 visual_context_tree_version)
{
    return adopt_ref(*new HitTestDisplayList(visual_context_tree_version));
}

HitTestDisplayList::HitTestDisplayList(u64 visual_context_tree_version)
    : m_visual_context_tree_version(visual_context_tree_version)
{
}

CSSPixelRect HitTestDisplayList::caret_line_rect_for_item(Item const& item) const
{
    if (!item.caret_line_rect.has_value())
        return item.caret_rect;

    auto rect = item.caret_rect;
    auto writing_mode = item.paintable->computed_values().writing_mode();
    auto line_rect = item.caret_line_rect.value();
    if (writing_mode_is_horizontal(writing_mode)) {
        rect.set_y(line_rect.y());
        rect.set_height(line_rect.height());
    } else {
        rect.set_x(line_rect.x());
        rect.set_width(line_rect.width());
    }
    return rect;
}

void HitTestDisplayList::append_box(PaintableBox const& paintable_box, Paintable& target, CSSPixelRect rect, VisualContextIndex visual_context_index, BorderRadiiData border_radii)
{
    Optional<size_t> caret_line_index;
    Optional<CSSPixelRect> caret_line_rect;
    if (auto const& line_box_data = paintable_box.containing_line_box_data(); line_box_data.has_value()) {
        caret_line_index = line_box_data->index;
        caret_line_rect = paintable_box.absolute_containing_line_box_rect();
    }

    auto item_index = m_items.size();
    m_items.append({
        .kind = ItemKind::Box,
        .paintable = target,
        .chrome_widget = {},
        .text_fragment = nullptr,
        .rect = rect,
        .caret_rect = rect,
        .caret_line_index = caret_line_index,
        .caret_line_rect = caret_line_rect,
        .visual_context_index = visual_context_index,
        .border_radii = border_radii,
    });
    add_item_to_spatial_index(item_index);
    add_item_to_caret_items(item_index);
}

void HitTestDisplayList::append_text_fragment(PaintableFragment const& fragment, VisualContextIndex visual_context_index)
{
    auto& fragment_paintable = const_cast<Paintable&>(fragment.paintable());
    if (!fragment_paintable.is_text_paintable() || !fragment_paintable.is_visible() || !fragment_paintable.visible_for_hit_testing())
        return;

    auto item_index = m_items.size();
    m_items.append({
        .kind = ItemKind::TextFragment,
        .paintable = fragment_paintable,
        .chrome_widget = {},
        .text_fragment = &fragment,
        .rect = fragment.absolute_rect(),
        .caret_rect = fragment.range_rect(Paintable::SelectionState::StartAndEnd, fragment.dom_start_offset_in_node(), fragment.dom_end_offset_in_node()),
        .caret_line_index = fragment.line_box_data().index,
        .caret_line_rect = fragment.absolute_line_box_rect(),
        .visual_context_index = visual_context_index,
        .border_radii = {},
    });
    add_item_to_spatial_index(item_index);
    add_item_to_caret_items(item_index);
}

void HitTestDisplayList::append_empty_editable(PaintableWithLines const& paintable, CSSPixelRect rect, VisualContextIndex visual_context_index)
{
    auto item_index = m_items.size();
    m_items.append({
        .kind = ItemKind::EmptyEditable,
        .paintable = const_cast<PaintableWithLines&>(paintable),
        .chrome_widget = {},
        .text_fragment = nullptr,
        .rect = rect,
        .caret_rect = rect,
        .caret_line_index = {},
        .caret_line_rect = {},
        .visual_context_index = visual_context_index,
        .border_radii = {},
    });
    add_item_to_spatial_index(item_index);
    add_item_to_caret_items(item_index);
}

void HitTestDisplayList::append_chrome_widget(PaintableBox const& paintable_box, ChromeWidget& chrome_widget, VisualContextIndex visual_context_index)
{
    auto item_index = m_items.size();
    m_items.append({
        .kind = ItemKind::ChromeWidget,
        .paintable = const_cast<PaintableBox&>(paintable_box),
        .chrome_widget = chrome_widget,
        .text_fragment = nullptr,
        .rect = {},
        .caret_rect = {},
        .caret_line_index = {},
        .caret_line_rect = {},
        .visual_context_index = visual_context_index,
        .border_radii = {},
    });
    add_item_to_spatial_index(item_index);
}

HitTestDisplayList::SpatialIndex& HitTestDisplayList::spatial_index_for(VisualContextIndex visual_context_index)
{
    auto index = visual_context_index.value();
    if (m_spatial_indexes.size() <= index)
        m_spatial_indexes.resize(index + 1);
    if (!m_spatial_indexes[index]) {
        m_spatial_indexes[index] = make<SpatialIndex>();
        m_used_visual_context_indices.append(visual_context_index);
    }
    return *m_spatial_indexes[index];
}

void HitTestDisplayList::add_item_to_spatial_index(size_t item_index)
{
    auto const& item = m_items[item_index];
    auto& spatial_index = spatial_index_for(item.visual_context_index);

    if (item.kind == ItemKind::ChromeWidget || item.rect.is_empty()) {
        spatial_index.unbucketed_items.append(item_index);
        return;
    }

    auto min_x = spatial_index_cell_for(item.rect.left());
    auto max_x = spatial_index_cell_for(item.rect.right());
    auto min_y = spatial_index_cell_for(item.rect.top());
    auto max_y = spatial_index_cell_for(item.rect.bottom());
    auto column_count = static_cast<i64>(max_x) - min_x + 1;
    auto row_count = static_cast<i64>(max_y) - min_y + 1;
    if (column_count <= 0 || row_count <= 0) {
        spatial_index.unbucketed_items.append(item_index);
        return;
    }
    auto cell_count = static_cast<u64>(column_count) * static_cast<u64>(row_count);
    if (cell_count > max_bucketed_cells_per_item) {
        spatial_index.unbucketed_items.append(item_index);
        return;
    }

    for (auto y = min_y; y <= max_y; ++y) {
        for (auto x = min_x; x <= max_x; ++x)
            spatial_index.cells.ensure(spatial_index_cell_key(x, y)).append(item_index);
    }
}

bool HitTestDisplayList::item_can_produce_caret_position(Item const& item) const
{
    switch (item.kind) {
    case ItemKind::TextFragment:
        return item.text_fragment && item.text_fragment->layout_node().dom_node();
    case ItemKind::EmptyEditable:
        return item.paintable->dom_node();
    case ItemKind::Box:
        return item.paintable->dom_node()
            && item.paintable->dom_node()->parent()
            && (item.paintable->layout_node().is_atomic_inline() || item.paintable->layout_node().is_replaced_box());
    case ItemKind::ChromeWidget:
        return false;
    }
    VERIFY_NOT_REACHED();
}

void HitTestDisplayList::add_item_to_caret_items(size_t item_index)
{
    auto const& item = m_items[item_index];
    if (item.caret_rect.is_empty() || !item_can_produce_caret_position(item))
        return;

    auto caret_item_index = m_caret_item_indices.size();
    m_caret_item_indices.append(item_index);

    auto writing_mode = item.paintable->computed_values().writing_mode();
    auto item_line_rect = caret_line_rect_for_item(item);
    if (!m_caret_lines.is_empty()) {
        auto& line = m_caret_lines.last();
        auto const& first_line_item = m_items[m_caret_item_indices[line.first_caret_item_index]];
        // Text fragments record their originating line box. Other caret-capable items, such as atomic inline boxes,
        // only join the previous caret line if their caret rects overlap in the block axis.
        auto same_recorded_line = first_line_item.caret_line_index.has_value()
            && item.caret_line_index.has_value()
            && *first_line_item.caret_line_index == *item.caret_line_index;
        auto same_inferred_line = !first_line_item.caret_line_index.has_value()
            && !item.caret_line_index.has_value()
            && rects_overlap_in_block_axis(line.rect, item.caret_rect, writing_mode);
        if (line.visual_context_index == item.visual_context_index
            && first_line_item.paintable->layout_node().containing_block() == item.paintable->layout_node().containing_block()
            && (same_recorded_line || same_inferred_line)) {
            line.rect.unite(item_line_rect);
            line.last_caret_item_index = caret_item_index;
            return;
        }
    }

    m_caret_lines.append({
        .rect = item_line_rect,
        .visual_context_index = item.visual_context_index,
        .first_caret_item_index = caret_item_index,
        .last_caret_item_index = caret_item_index,
    });
}

Optional<CSSPixelPoint> HitTestDisplayList::local_point_for_visual_context(VisualContextIndex visual_context_index, CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel) const
{
    if (!visual_context_index.value())
        return point;

    auto pixel_ratio = static_cast<float>(device_pixels_per_css_pixel);
    auto const& visual_context_tree = viewport_paintable.visual_context_tree();
    auto result = visual_context_tree.transform_point_for_hit_test(visual_context_index, point.to_type<float>() * pixel_ratio, viewport_paintable.scroll_state_snapshot());
    if (!result.has_value())
        return {};
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

bool HitTestDisplayList::item_contains(Item const& item, CSSPixelPoint local_point, ChromeMetrics const& chrome_metrics) const
{
    switch (item.kind) {
    case ItemKind::Box:
        return item.rect.contains(local_point) && item.border_radii.contains(local_point, item.rect);
    case ItemKind::TextFragment:
        return item.rect.contains(local_point);
    case ItemKind::EmptyEditable:
        return item.rect.contains(local_point);
    case ItemKind::ChromeWidget:
        return item.chrome_widget && item.chrome_widget->contains(local_point, chrome_metrics);
    }
    VERIFY_NOT_REACHED();
}

DOM::Node const* HitTestDisplayList::item_dom_node(Item const& item) const
{
    switch (item.kind) {
    case ItemKind::Box:
    case ItemKind::EmptyEditable:
    case ItemKind::ChromeWidget:
        return item.paintable->dom_node();
    case ItemKind::TextFragment:
        return item.text_fragment ? item.text_fragment->layout_node().dom_node() : nullptr;
    }
    VERIFY_NOT_REACHED();
}

HitTestResult HitTestDisplayList::hit_test_result_for_item(Item const& item, CSSPixelPoint local_point) const
{
    switch (item.kind) {
    case ItemKind::Box:
        return HitTestResult { .paintable = item.paintable };
    case ItemKind::TextFragment:
        VERIFY(item.text_fragment);
        return HitTestResult {
            .paintable = item.paintable,
            .index_in_node = item.text_fragment->index_in_node_for_point(local_point),
        };
    case ItemKind::EmptyEditable:
        return HitTestResult {
            .paintable = item.paintable,
            .index_in_node = 0,
        };
    case ItemKind::ChromeWidget:
        return HitTestResult { .paintable = item.paintable, .chrome_widget = item.chrome_widget };
    }
    VERIFY_NOT_REACHED();
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_item(Item const& item, CSSPixelPoint local_point, CaretPositionType type) const
{
    switch (item.kind) {
    case ItemKind::TextFragment: {
        VERIFY(item.text_fragment);
        auto const& fragment = *item.text_fragment;
        auto const* fragment_dom_node = fragment.layout_node().dom_node();
        if (!fragment_dom_node)
            return {};

        auto index_in_node = [&] {
            switch (type) {
            case CaretPositionType::Before:
                return fragment.dom_start_offset_in_node();
            case CaretPositionType::After:
                return fragment.dom_end_offset_in_node();
            case CaretPositionType::Closest:
                return fragment.index_in_node_for_point(local_point);
            }
            VERIFY_NOT_REACHED();
        }();

        return CaretPosition {
            .paintable = item.paintable,
            .boundary = { const_cast<DOM::Node&>(*fragment_dom_node), static_cast<WebIDL::UnsignedLong>(index_in_node) },
        };
    }
    case ItemKind::EmptyEditable: {
        auto dom_node = item.paintable->dom_node();
        if (!dom_node)
            return {};
        return CaretPosition {
            .paintable = item.paintable,
            .boundary = { *dom_node, 0 },
        };
    }
    case ItemKind::Box: {
        auto dom_node = item.paintable->dom_node();
        if (!dom_node || !dom_node->parent())
            return {};

        auto before_boundary = DOM::BoundaryPoint { *dom_node->parent(), static_cast<WebIDL::UnsignedLong>(dom_node->index()) };
        auto after_boundary = DOM::BoundaryPoint { *dom_node->parent(), static_cast<WebIDL::UnsignedLong>(dom_node->index() + 1) };
        auto point_is_before_box = [&] {
            switch (type) {
            case CaretPositionType::Before:
                return true;
            case CaretPositionType::After:
                return false;
            case CaretPositionType::Closest:
                return local_point_is_before_box(*item.paintable, item.rect, local_point);
            }
            VERIFY_NOT_REACHED();
        }();
        return CaretPosition {
            .paintable = item.paintable,
            .boundary = point_is_before_box ? before_boundary : after_boundary,
            .secondary_boundary = point_is_before_box ? after_boundary : before_boundary,
        };
    }
    case ItemKind::ChromeWidget:
        return {};
    }
    VERIFY_NOT_REACHED();
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_hit_container(Item const& item) const
{
    auto dom_node = item_dom_node(item);
    if (!dom_node)
        return {};

    return CaretPosition {
        .paintable = item.paintable,
        .boundary = { const_cast<DOM::Node&>(*dom_node), 0 },
    };
}

Optional<CaretPosition> HitTestDisplayList::caret_position_for_line(CaretLine const& line, CSSPixelPoint local_point) const
{
    auto const& first_item = m_items[m_caret_item_indices[line.first_caret_item_index]];
    auto writing_mode = first_item.paintable->computed_values().writing_mode();
    auto block_coordinate = block_axis_coordinate(local_point, writing_mode);
    if (block_coordinate < block_axis_start(line.rect, writing_mode) || block_coordinate >= block_axis_end(line.rect, writing_mode)) {
        auto type = local_point_is_before_line(*first_item.paintable, line.rect, local_point) ? CaretPositionType::Before : CaretPositionType::After;
        auto caret_item_index = type == CaretPositionType::Before ? line.first_caret_item_index : line.last_caret_item_index;
        return caret_position_for_item(m_items[m_caret_item_indices[caret_item_index]], local_point, type);
    }

    Optional<size_t> closest_item_index;
    auto closest_block_distance = CSSPixels::max();
    auto closest_inline_distance = CSSPixels::max();

    for (auto caret_item_index = line.first_caret_item_index; caret_item_index <= line.last_caret_item_index; ++caret_item_index) {
        auto item_index = m_caret_item_indices[caret_item_index];
        auto const& item = m_items[item_index];
        auto writing_mode = item.paintable->computed_values().writing_mode();
        auto block_distance = block_axis_distance_to_line_rect(caret_line_rect_for_item(item), local_point, writing_mode);
        auto inline_distance = inline_axis_distance_to_rect(item.caret_rect, local_point, writing_mode);
        if (!closest_item_index.has_value()
            || caret_line_is_better_candidate(block_distance, inline_distance, closest_block_distance, closest_inline_distance, caret_item_block_axis_compare_slop)) {
            closest_item_index = item_index;
            closest_block_distance = block_distance;
            closest_inline_distance = inline_distance;
        }
    }

    if (!closest_item_index.has_value())
        return {};
    return caret_position_for_item(m_items[*closest_item_index], local_point);
}

bool HitTestDisplayList::line_contains_descendant_of(CaretLine const& line, DOM::Node const& ancestor) const
{
    for (auto caret_item_index = line.first_caret_item_index; caret_item_index <= line.last_caret_item_index; ++caret_item_index) {
        auto item_index = m_caret_item_indices[caret_item_index];
        auto const& item = m_items[item_index];
        if (auto const* dom_node = item_dom_node(item); dom_node && ancestor.is_inclusive_ancestor_of(*dom_node))
            return true;
    }
    return false;
}

void HitTestDisplayList::find_topmost_item_in_list(Vector<size_t> const& item_indices, CSSPixelPoint local_point, ChromeMetrics const& chrome_metrics, Optional<size_t>& topmost_item_index) const
{
    for (auto item_index : item_indices.in_reverse()) {
        if (topmost_item_index.has_value() && item_index <= *topmost_item_index)
            return;
        if (!item_contains(m_items[item_index], local_point, chrome_metrics))
            continue;
        topmost_item_index = item_index;
        return;
    }
}

void HitTestDisplayList::find_topmost_caret_item_in_list(Vector<size_t> const& item_indices, CSSPixelPoint local_point, ChromeMetrics const& chrome_metrics, Optional<size_t>& topmost_item_index) const
{
    for (auto item_index : item_indices.in_reverse()) {
        if (topmost_item_index.has_value() && item_index <= *topmost_item_index)
            return;
        auto const& item = m_items[item_index];
        if (!item_can_produce_caret_position(item))
            continue;
        if (!item_contains(item, local_point, chrome_metrics))
            continue;
        topmost_item_index = item_index;
        return;
    }
}

void HitTestDisplayList::find_items_in_list(Vector<size_t> const& item_indices, CSSPixelPoint local_point, ChromeMetrics const& chrome_metrics, Vector<size_t>& hit_item_indices) const
{
    for (auto item_index : item_indices) {
        if (item_contains(m_items[item_index], local_point, chrome_metrics))
            hit_item_indices.append(item_index);
    }
}

Optional<CaretPosition> HitTestDisplayList::caret_position_from_point(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    if (m_visual_context_tree_version != viewport_paintable.visual_context_tree().version())
        return {};

    // First find both the topmost hit-test item and the topmost item that can directly produce a caret.
    // Non-caret items are still needed to keep later line fallback scoped to the hit content.
    Optional<size_t> topmost_item_index;
    Optional<CSSPixelPoint> topmost_item_local_point;
    Optional<size_t> topmost_hit_item_index;
    Optional<CSSPixelPoint> topmost_hit_item_local_point;
    for (auto visual_context_index : m_used_visual_context_indices) {
        auto const& spatial_index = m_spatial_indexes[visual_context_index.value()];
        VERIFY(spatial_index);

        auto local_point = local_point_for_visual_context(visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel);
        if (!local_point.has_value())
            continue;

        auto previous_topmost_item_index = topmost_item_index;
        auto previous_topmost_hit_item_index = topmost_hit_item_index;
        find_topmost_item_in_list(spatial_index->unbucketed_items, *local_point, chrome_metrics, topmost_hit_item_index);
        find_topmost_caret_item_in_list(spatial_index->unbucketed_items, *local_point, chrome_metrics, topmost_item_index);

        auto x = spatial_index_cell_for(local_point->x());
        auto y = spatial_index_cell_for(local_point->y());
        if (auto bucket = spatial_index->cells.get(spatial_index_cell_key(x, y)); bucket.has_value()) {
            find_topmost_item_in_list(*bucket, *local_point, chrome_metrics, topmost_hit_item_index);
            find_topmost_caret_item_in_list(*bucket, *local_point, chrome_metrics, topmost_item_index);
        }

        if (topmost_item_index != previous_topmost_item_index)
            topmost_item_local_point = local_point;
        if (topmost_hit_item_index != previous_topmost_hit_item_index)
            topmost_hit_item_local_point = local_point;
    }

    if (topmost_item_index.has_value() && (!topmost_hit_item_index.has_value() || *topmost_item_index >= *topmost_hit_item_index)) {
        VERIFY(topmost_item_local_point.has_value());
        if (auto caret_position = caret_position_for_item(m_items[*topmost_item_index], *topmost_item_local_point); caret_position.has_value())
            return caret_position;
    }

    DOM::Node const* line_scope_dom_node = nullptr;
    if (topmost_hit_item_index.has_value()) {
        auto const& topmost_hit_item = m_items[*topmost_hit_item_index];
        if (!item_can_produce_caret_position(topmost_hit_item))
            line_scope_dom_node = item_dom_node(topmost_hit_item);
    }

    Optional<size_t> closest_line_index;
    Optional<CSSPixelPoint> closest_line_local_point;
    auto closest_line_distance = CSSPixels::max();

    for (size_t line_index = 0; line_index < m_caret_lines.size(); ++line_index) {
        auto const& line = m_caret_lines[line_index];
        if (line_scope_dom_node && !line_contains_descendant_of(line, *line_scope_dom_node))
            continue;

        auto local_point = local_point_for_visual_context(line.visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel);
        if (!local_point.has_value())
            continue;

        auto first_item_index = m_caret_item_indices[line.first_caret_item_index];
        auto const& first_item = m_items[first_item_index];
        auto distance = block_axis_distance_to_rect(line.rect, *local_point, first_item.paintable->computed_values().writing_mode());
        if (!closest_line_index.has_value() || distance <= closest_line_distance) {
            closest_line_index = line_index;
            closest_line_local_point = local_point;
            closest_line_distance = distance;
        }
    }

    if (!closest_line_index.has_value()) {
        if (topmost_hit_item_index.has_value())
            return caret_position_for_hit_container(m_items[*topmost_hit_item_index]);
        return {};
    }
    VERIFY(closest_line_local_point.has_value());
    auto caret_position = caret_position_for_line(m_caret_lines[*closest_line_index], *closest_line_local_point);
    if (!caret_position.has_value())
        return {};

    if (topmost_hit_item_index.has_value()) {
        auto const& topmost_hit_item = m_items[*topmost_hit_item_index];
        if (auto const* topmost_hit_dom_node = item_dom_node(topmost_hit_item); topmost_hit_dom_node && !topmost_hit_dom_node->is_inclusive_ancestor_of(*caret_position->boundary.node)) {
            if (item_can_produce_caret_position(topmost_hit_item)) {
                VERIFY(topmost_hit_item_local_point.has_value());
                return caret_position_for_item(topmost_hit_item, *topmost_hit_item_local_point);
            }
            return {};
        }
    }

    return caret_position;
}

Optional<HitTestResult> HitTestDisplayList::hit_test(CSSPixelPoint point, HitTestType type, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics) const
{
    (void)type;

    if (m_visual_context_tree_version != viewport_paintable.visual_context_tree().version())
        return {};

    Optional<size_t> topmost_item_index;
    Optional<CSSPixelPoint> topmost_item_local_point;

    for (auto visual_context_index : m_used_visual_context_indices) {
        auto const& spatial_index = m_spatial_indexes[visual_context_index.value()];
        VERIFY(spatial_index);

        auto local_point = local_point_for_visual_context(visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel);
        if (!local_point.has_value())
            continue;

        auto previous_topmost_item_index = topmost_item_index;
        find_topmost_item_in_list(spatial_index->unbucketed_items, *local_point, chrome_metrics, topmost_item_index);

        auto x = spatial_index_cell_for(local_point->x());
        auto y = spatial_index_cell_for(local_point->y());
        if (auto bucket = spatial_index->cells.get(spatial_index_cell_key(x, y)); bucket.has_value())
            find_topmost_item_in_list(*bucket, *local_point, chrome_metrics, topmost_item_index);

        if (topmost_item_index != previous_topmost_item_index)
            topmost_item_local_point = local_point;
    }

    if (!topmost_item_index.has_value())
        return {};

    auto const& item = m_items[*topmost_item_index];
    if (!topmost_item_local_point.has_value()) {
        topmost_item_local_point = local_point_for_visual_context(item.visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel);
        if (!topmost_item_local_point.has_value()) {
            VERIFY_NOT_REACHED();
        }
    }

    return hit_test_result_for_item(item, *topmost_item_local_point);
}

TraversalDecision HitTestDisplayList::hit_test_all(CSSPixelPoint point, ViewportPaintable const& viewport_paintable, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (m_visual_context_tree_version != viewport_paintable.visual_context_tree().version())
        return TraversalDecision::Continue;

    Vector<size_t> hit_item_indices;
    for (auto visual_context_index : m_used_visual_context_indices) {
        auto const& spatial_index = m_spatial_indexes[visual_context_index.value()];
        VERIFY(spatial_index);

        auto local_point = local_point_for_visual_context(visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel);
        if (!local_point.has_value())
            continue;

        find_items_in_list(spatial_index->unbucketed_items, *local_point, chrome_metrics, hit_item_indices);

        auto x = spatial_index_cell_for(local_point->x());
        auto y = spatial_index_cell_for(local_point->y());
        if (auto bucket = spatial_index->cells.get(spatial_index_cell_key(x, y)); bucket.has_value())
            find_items_in_list(*bucket, *local_point, chrome_metrics, hit_item_indices);
    }

    quick_sort(hit_item_indices, [](auto a, auto b) { return a > b; });

    Optional<size_t> previous_item_index;
    for (auto item_index : hit_item_indices) {
        if (previous_item_index == item_index)
            continue;
        previous_item_index = item_index;

        auto const& item = m_items[item_index];
        auto local_point = local_point_for_visual_context(item.visual_context_index, point, viewport_paintable, device_pixels_per_css_pixel);
        if (!local_point.has_value())
            continue;
        if (callback(hit_test_result_for_item(item, *local_point)) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

}
