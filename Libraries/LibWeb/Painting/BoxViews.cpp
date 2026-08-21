/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

bool has_committed_box(Layout::Node const& node)
{
    return node.paintable_ptr();
}

#define FORWARD_BOX_VIEW(return_type, name, zero_value) \
    return_type name(Layout::Node const& node)          \
    {                                                   \
        auto const* paintable = node.paintable_ptr();   \
        if (!paintable)                                 \
            return zero_value;                          \
        return paintable->name();                       \
    }

FORWARD_BOX_VIEW(CSSPixelRect, absolute_rect, {})
FORWARD_BOX_VIEW(CSSPixelRect, absolute_padding_box_rect, {})
FORWARD_BOX_VIEW(CSSPixelRect, absolute_border_box_rect, {})
FORWARD_BOX_VIEW(CSSPixelPoint, absolute_position, {})
FORWARD_BOX_VIEW(CSSPixels, absolute_x, {})
FORWARD_BOX_VIEW(CSSPixels, absolute_y, {})
FORWARD_BOX_VIEW(CSSPixelPoint, offset, {})
FORWARD_BOX_VIEW(CSSPixelSize, content_size, {})
FORWARD_BOX_VIEW(CSSPixels, content_width, {})
FORWARD_BOX_VIEW(CSSPixels, content_height, {})
FORWARD_BOX_VIEW(CSSPixels, border_box_width, {})
FORWARD_BOX_VIEW(CSSPixels, border_box_height, {})
FORWARD_BOX_VIEW(BoxModelMetrics, box_model, {})
FORWARD_BOX_VIEW(CSSPixels, outline_offset, {})
FORWARD_BOX_VIEW(CSSPixelRect, transform_reference_box, {})
FORWARD_BOX_VIEW(Optional<CSSPixelRect>, scrollable_overflow_rect, {})
FORWARD_BOX_VIEW(bool, has_scrollable_overflow, false)
FORWARD_BOX_VIEW(Optional<Paintable::OverflowData>, overflow_data, {})
FORWARD_BOX_VIEW(Optional<Paintable::CachedOverflowData>, cached_overflow_data, {})

FORWARD_BOX_VIEW(bool, is_visible, false)
FORWARD_BOX_VIEW(bool, visible_for_hit_testing, false)
FORWARD_BOX_VIEW(bool, has_stacking_context, false)
FORWARD_BOX_VIEW(Optional<int>, effective_z_index, {})
FORWARD_BOX_VIEW(CSS::Display, display, {})
FORWARD_BOX_VIEW(bool, is_positioned, false)
FORWARD_BOX_VIEW(bool, is_fixed_position, false)
FORWARD_BOX_VIEW(bool, is_sticky_position, false)
FORWARD_BOX_VIEW(bool, is_absolutely_positioned, false)
FORWARD_BOX_VIEW(bool, is_floating, false)
FORWARD_BOX_VIEW(bool, is_inline, false)
FORWARD_BOX_VIEW(bool, has_css_transform, false)
FORWARD_BOX_VIEW(bool, has_non_invertible_css_transform, false)
FORWARD_BOX_VIEW(bool, uses_collapsing_borders_model, false)
FORWARD_BOX_VIEW(SelectionState, selection_state, {})
FORWARD_BOX_VIEW(CSS::StyleRecordID, style_record_identity, {})
FORWARD_BOX_VIEW(StringView, class_name, {})
FORWARD_BOX_VIEW(String, debug_description, {})
FORWARD_BOX_VIEW(bool, is_navigable_container_viewport_paintable, false)
FORWARD_BOX_VIEW(bool, is_viewport_paintable, false)
FORWARD_BOX_VIEW(bool, is_paintable_with_lines, false)
FORWARD_BOX_VIEW(bool, is_inline_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_svg_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_path_paintable, false)
FORWARD_BOX_VIEW(bool, is_svg_foreign_object_paintable, false)

FORWARD_BOX_VIEW(bool, has_accumulated_visual_context, false)
FORWARD_BOX_VIEW(VisualContextIndex, accumulated_visual_context_index, {})
FORWARD_BOX_VIEW(VisualContextIndex, accumulated_visual_context_for_descendants_index, {})
FORWARD_BOX_VIEW(Optional<VisualContextIndex>, fixed_background_visual_context, {})
FORWARD_BOX_VIEW(VisualContextIndex, enclosing_scroll_node_index, {})
FORWARD_BOX_VIEW(VisualContextIndex, own_scroll_node_index, {})

FORWARD_BOX_VIEW(Gfx::Path const*, committed_svg_path, nullptr)
FORWARD_BOX_VIEW(CSSPixelSize, svg_viewport_size, {})
FORWARD_BOX_VIEW(Optional<Gfx::AffineTransform>, svg_viewport_transform, {})
FORWARD_BOX_VIEW(Optional<UsedGridTrackList>, used_values_for_grid_template_columns, {})
FORWARD_BOX_VIEW(Optional<UsedGridTrackList>, used_values_for_grid_template_rows, {})

FORWARD_BOX_VIEW(CSSPixelPoint, box_type_agnostic_position, {})
FORWARD_BOX_VIEW(bool, should_paint_cursor, false)
FORWARD_BOX_VIEW(Paintable::SelectionStyle, selection_style, {})
FORWARD_BOX_VIEW(StickyInsets, sticky_insets, {})
FORWARD_BOX_VIEW(bool, has_sticky_insets, false)

#undef FORWARD_BOX_VIEW

Optional<CSS::BorderData> outline_data(Layout::Node const& node, CSS::ComputedValues const& computed_values)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->outline_data(computed_values) : Optional<CSS::BorderData> {};
}

CSSPixelRect transform_rect_to_viewport(Layout::Node const& node, CSSPixelRect const& rect, AccumulatedVisualContextTree::IncludeVisualViewportTransform include_visual_viewport_transform)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_rect_to_viewport(rect, include_visual_viewport_transform) : CSSPixelRect {};
}

Optional<CSSPixelPoint> transform_point_to_local(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_point_to_local(position) : Optional<CSSPixelPoint> {};
}

Optional<CSSPixelPoint> transform_point_to_local_for_descendants(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_point_to_local_for_descendants(position) : Optional<CSSPixelPoint> {};
}

CSSPixelPoint inverse_transform_point(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->inverse_transform_point(position) : CSSPixelPoint {};
}

CSSPixelPoint transform_to_local_coordinates(Layout::Node const& node, CSSPixelPoint position)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->transform_to_local_coordinates(position) : CSSPixelPoint {};
}

Optional<String> grid_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->grid_layout_json(container_node_id) : Optional<String> {};
}

Optional<String> flex_layout_json(Layout::Node const& node, UniqueNodeID container_node_id)
{
    auto const* paintable = node.paintable_ptr();
    return paintable ? paintable->flex_layout_json(container_node_id) : Optional<String> {};
}

Paintable::SelectionStyle selection_style_for_node(Layout::Node const& node, GC::Ptr<DOM::Node const> dom_node)
{
    return Paintable::selection_style_for_node(node, dom_node);
}

void set_needs_repaint(Layout::Node const& node, InvalidateDisplayList should_invalidate_display_list)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->set_needs_repaint(should_invalidate_display_list);
}

void invalidate_paint_cache(Layout::Node const& node)
{
    if (auto const* paintable = node.paintable_ptr())
        paintable->invalidate_paint_cache();
}

void repaint_after_style_change(Layout::Node const& node, CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->repaint_after_style_change(invalidation);
}

void invalidate_stacking_context(Layout::Node const& node)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->invalidate_stacking_context();
}

void clear_overflow_data(Layout::Node const& node)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->clear_overflow_data();
}

void clear_cached_overflow_data(Layout::Node const& node)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->clear_cached_overflow_data();
}

void set_sticky_insets(Layout::Node const& node, OwnPtr<StickyInsets> sticky_insets)
{
    if (auto* paintable = const_cast<Paintable*>(node.paintable_ptr()))
        paintable->set_sticky_insets(move(sticky_insets));
}

void inline_piece_border_box_rects(Layout::Node const& node, Vector<CSSPixelRect>& rects)
{
    auto const* paintable = node.paintable_ptr();
    if (!paintable)
        return;
    Layout::RustFFI::layout_arena_inline_paintable_piece_border_box_rects(
        paintable->rust_arena().handle(), paintable->rust_slot(), &rects,
        [](void* context, Layout::RustFFI::FfiCssPixelRect rect) {
            static_cast<Vector<CSSPixelRect>*>(context)->append({
                CSSPixels::from_raw(rect.x),
                CSSPixels::from_raw(rect.y),
                CSSPixels::from_raw(rect.width),
                CSSPixels::from_raw(rect.height),
            });
        });
}

CSSPixelPoint cumulative_scroll_compensation(Layout::Node const& node)
{
    auto const* paintable = node.paintable_ptr();
    if (!paintable)
        return {};
    auto index = paintable->enclosing_scroll_node_index();
    if (!index.value())
        return {};
    return paintable->document().paintable()->cumulative_scroll_offset_for_node(index);
}

}
