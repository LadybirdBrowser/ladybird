/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>

namespace Web::Painting {

struct ImagePaint;

inline Layout::RustFFI::FfiCssPixelRect to_ffi_css_pixel_rect(CSSPixelRect const& rect)
{
    return { rect.x().raw_value(), rect.y().raw_value(), rect.width().raw_value(), rect.height().raw_value() };
}

inline CSSPixelRect from_ffi_css_pixel_rect(Layout::RustFFI::FfiCssPixelRect const& rect)
{
    return { CSSPixels::from_raw(rect.x), CSSPixels::from_raw(rect.y), CSSPixels::from_raw(rect.width), CSSPixels::from_raw(rect.height) };
}

WEB_API void rust_build_stacking_context_tree(ViewportPaintable&);
WEB_API void dump_stacking_context_tree(StringBuilder&, ViewportPaintable const&);

WEB_API bool rust_assign_accumulated_visual_contexts(ViewportPaintable&, bool forced_incompatible_rebuild);
WEB_API AccumulatedVisualContextTree materialize_rust_main_visual_context_tree(ViewportPaintable&);
WEB_API void patch_rust_visual_context_nodes(ViewportPaintable&, AccumulatedVisualContextTree&, size_t begin, size_t end);
WEB_API bool rust_update_accumulated_visual_context_values(ViewportPaintable&, Paintable&);
WEB_API Optional<TransformData> rust_compute_css_transform(Paintable const&, double pixel_ratio);
WEB_API CSS::ResolvedImage rust_resolve_gradient_for_size(CSS::StyleValue const&, Layout::NodeWithStyle const&, CSSPixelSize);
WEB_API void rust_update_visual_viewport_transform(ViewportPaintable&);
WEB_API void rust_refresh_scroll_state(ViewportPaintable&);
WEB_API ScrollStateSnapshot rust_scroll_state_snapshot(ViewportPaintable&);
WEB_API CSSPixelPoint rust_cumulative_scroll_offset_for_node(ViewportPaintable const&, VisualContextIndex scroll_node_index);
WEB_API void mirror_rust_refresh_sticky_constraints(ViewportPaintable&);
WEB_API void mirror_rust_clear_scroll_state(ViewportPaintable&);
WEB_API void mirror_rust_set_needs_to_refresh_scroll_state(ViewportPaintable&, bool);
WEB_API void mirror_rust_clear_visual_context_tree(ViewportPaintable&);
WEB_API void mirror_rust_reset_visual_context_state(ViewportPaintable&);
WEB_API void mirror_rust_clear_paint_cache_sources(ViewportPaintable&);
WEB_API void mirror_rust_invalidate_paint_cache(Paintable const&);
struct InspectorOverlayInputs {
    Paintable const* highlighted_paintable { nullptr };
    Color tooltip_color;
    Color tooltip_text_color;
    Color tooltip_border_color;
    struct GridHighlight {
        Paintable const* paintable { nullptr };
        GridInspectorOverlayOptions options;
    };
    struct FlexHighlight {
        Paintable const* paintable { nullptr };
        FlexboxInspectorOverlayOptions options;
    };
    Vector<GridHighlight> grid_highlights;
    Vector<FlexHighlight> flex_highlights;
    Optional<CSSPixelRect> caret_debug_rect;
};

WEB_API RefPtr<DisplayList> record_rust_display_list(ViewportPaintable&, DisplayList const& placeholder_display_list, DisplayListResourceStorage&, DisplayListRecordingContext&, HTML::PaintConfig const&, InspectorOverlayInputs const&);

WEB_API NonnullRefPtr<DisplayList> record_image_paint_display_list(ImagePaint const&, Gfx::FloatRect dest_rect, CSS::ImageRendering, double device_pixels_per_css_pixel, AccumulatedVisualContextTree const&, DisplayListResourceStorage&);

}
