/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/AffineTransform.h>
#include <LibGfx/Forward.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>

namespace Web::Painting {

struct CaretPaint {
    CSSPixelRect rect;
    Color color;
};

WEB_API void set_paint_viewport_scrollbars(bool enabled);
bool should_paint_viewport_scrollbars();
ResolvedCSSFilter resolve_css_filter(CSS::ComputedFilterView, Layout::NodeWithStyle const&);

bool body_background_is_propagated_to_root(Layout::NodeWithStyle const&);

Layout::RustFFI::NodeSlotId committed_row_slot(Layout::Node const&);
Layout::RustFFI::NodeSlotId viewport_row_slot(DOM::Document const&);
Layout::RustFFI::PaintableData const* committed_row(Layout::Node const&);

WEB_API bool has_committed_box(Layout::Node const&);
WEB_API Layout::Node* layout_node_for_committed_slot(Layout::NodeArena&, Layout::RustFFI::NodeSlotId);

WEB_API CSSPixelRect absolute_rect(Layout::Node const&);
WEB_API CSSPixelRect absolute_padding_box_rect(Layout::Node const&);
WEB_API CSSPixelRect absolute_border_box_rect(Layout::Node const&);
WEB_API CSSPixelPoint absolute_position(Layout::Node const&);
WEB_API CSSPixelPoint offset(Layout::Node const&);
WEB_API CSSPixelSize content_size(Layout::Node const&);
WEB_API CSSPixels content_width(Layout::Node const&);
WEB_API CSSPixels content_height(Layout::Node const&);
WEB_API CSSPixels border_box_width(Layout::Node const&);
WEB_API CSSPixels border_box_height(Layout::Node const&);
WEB_API BoxModelMetrics box_model(Layout::Node const&);
WEB_API Optional<CSS::BorderData> outline_data(Layout::Node const&, CSS::ComputedValues const&);
WEB_API CSSPixels outline_offset(Layout::Node const&);
WEB_API CSSPixelRect transform_reference_box(Layout::Node const&);
WEB_API Optional<CSSPixelRect> scrollable_overflow_rect(Layout::Node const&);
WEB_API bool has_scrollable_overflow(Layout::Node const&);
WEB_API Optional<OverflowData> overflow_data(Layout::Node const&);
WEB_API Optional<CSSPixelRect> mask_area(Layout::Node const&);
WEB_API Optional<Gfx::MaskKind> mask_type(Layout::Node const&);
WEB_API Optional<CSSPixelRect> clip_area(Layout::Node const&);

WEB_API bool is_visible(Layout::Node const&);
WEB_API bool visible_for_hit_testing(Layout::Node const&);
WEB_API bool has_stacking_context(Layout::Node const&);
WEB_API Optional<int> effective_z_index(Layout::Node const&);
WEB_API CSS::Display display(Layout::Node const&);
WEB_API bool is_positioned(Layout::Node const&);
WEB_API bool is_fixed_position(Layout::Node const&);
WEB_API bool has_css_transform(Layout::Node const&);
WEB_API bool uses_collapsing_borders_model(Layout::Node const&);
WEB_API SelectionState selection_state(Layout::Node const&);
WEB_API CSS::StyleRecordID style_record_identity(Layout::Node const&);
WEB_API bool is_navigable_container_viewport_paintable(Layout::Node const&);
WEB_API bool is_viewport_paintable(Layout::Node const&);
WEB_API bool is_paintable_with_lines(Layout::Node const&);
WEB_API bool is_inline_paintable(Layout::Node const&);
WEB_API bool is_svg_paintable(Layout::Node const&);
WEB_API bool is_svg_svg_paintable(Layout::Node const&);
WEB_API bool is_svg_path_paintable(Layout::Node const&);

WEB_API CSSPixelRect transform_rect_to_viewport(Layout::Node const&, CSSPixelRect const&, AccumulatedVisualContextTree::IncludeVisualViewportTransform = AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes);
WEB_API Optional<CSSPixelPoint> transform_point_to_local(Layout::Node const&, CSSPixelPoint);
WEB_API CSSPixelPoint inverse_transform_point(Layout::Node const&, CSSPixelPoint);
WEB_API CSSPixelPoint transform_to_local_coordinates(Layout::Node const&, CSSPixelPoint);

WEB_API bool has_accumulated_visual_context(Layout::Node const&);
WEB_API ContextRef accumulated_visual_context(Layout::Node const&);
WEB_API ContextRef accumulated_visual_context_for_descendants(Layout::Node const&);
WEB_API Optional<ContextRef> fixed_background_visual_context(Layout::Node const&);
WEB_API SpatialNodeIndex enclosing_scroll_node_index(Layout::Node const&);
WEB_API SpatialNodeIndex own_scroll_node_index(Layout::Node const&);

WEB_API Gfx::Path const* committed_svg_path(Layout::Node const&);
WEB_API CSSPixelSize svg_viewport_size(Layout::Node const&);
WEB_API Optional<Gfx::AffineTransform> svg_viewport_transform(Layout::Node const&);
WEB_API Optional<UsedGridTrackList> used_values_for_grid_template_columns(Layout::Node const&);
WEB_API Optional<UsedGridTrackList> used_values_for_grid_template_rows(Layout::Node const&);
WEB_API Optional<String> grid_layout_json(Layout::Node const&, UniqueNodeID);
WEB_API Optional<String> flex_layout_json(Layout::Node const&, UniqueNodeID);

WEB_API CSSPixelPoint box_type_agnostic_position(Layout::Node const&);
WEB_API bool should_paint_cursor(Layout::Node const&);
WEB_API Layout::Node const* nearest_self_painting_inline_box(Layout::Node const&);
WEB_API bool has_content(Layout::Node const&);
WEB_API CSSPixelRect caret_rect_for_child_offset(Layout::Node const&, size_t offset);
WEB_API Optional<CaretPaint> resolve_caret_paint(Layout::Node const& block, Layout::Node const* owner_inline);
WEB_API Optional<CaretPaint> resolve_empty_editable_caret_paint(Layout::Node const&);
WEB_API SelectionStyle selection_style(Layout::Node const&);
WEB_API SelectionStyle selection_style_for_node(Layout::Node const&, GC::Ptr<DOM::Node const>);

WEB_API void set_needs_repaint(Layout::Node const&, InvalidateDisplayList = InvalidateDisplayList::Yes);
WEB_API void invalidate_paint_cache(Layout::Node const&);
WEB_API void repaint_after_style_change(Layout::Node const&, CSS::RequiredInvalidationAfterStyleChange const&);
WEB_API void invalidate_stacking_context(Layout::Node const&);
WEB_API void clear_overflow_data(Layout::Node const&);
WEB_API void clear_cached_overflow_data(Layout::Node const&);

WEB_API void inline_piece_border_box_rects(Layout::Node const&, Vector<CSSPixelRect>&);
WEB_API CSSPixelPoint cumulative_scroll_compensation(Layout::Node const&);

}
