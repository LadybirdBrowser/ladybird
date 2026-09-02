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
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>
#include <LibWeb/Painting/PaintableTypes.h>

namespace Web::Painting {

struct ImagePaint;
struct ImagePaintRequest;

WEB_API void dump_stacking_context_tree(StringBuilder&, DOM::Document const&);

WEB_API Layout::RustFFI::FfiVisualContextUpdateOutcome rust_update_accumulated_visual_contexts(DOM::Document&);
WEB_API Vector<u32> rust_owned_visual_context_node_indices(Layout::Node const&, Layout::RustFFI::FfiVisualContextBoxNodeList);
WEB_API Vector<u32> rust_visual_animation_target_node_indices(Layout::Node const&, AccumulatedVisualContextTree const&, bool targets_are_frames);
WEB_API void const* retain_rust_main_visual_context_tree(DOM::Document const&);
WEB_API CSSPixelRect rust_apply_css_transform_to_rect(Layout::Node const&, CSSPixelRect const&);
WEB_API Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Layout::Node const&);
WEB_API void rust_measure_scrollable_overflow(Layout::Node const&);
WEB_API Layout::RustFFI::FfiScrollableOverflowUpdateOutcome rust_update_scrollable_overflow(DOM::Document&, bool handled_by_full_layout_commit);
WEB_API void rust_update_visual_viewport_transform(DOM::Document&);
WEB_API void rust_refresh_scroll_state(DOM::Document&);
WEB_API ScrollStateSnapshot rust_scroll_state_snapshot(DOM::Document&);
WEB_API bool mirror_rust_refresh_sticky_constraints(DOM::Document&);
WEB_API void mirror_rust_clear_scroll_state(DOM::Document&);
WEB_API void mirror_rust_set_needs_to_refresh_scroll_state(DOM::Document&, bool);
WEB_API void mirror_rust_invalidate_paint_cache(Layout::Node const&);
WEB_API void rust_invalidate_propagated_text_decoration_caches(Layout::Node const&);
struct InspectorOverlayInputs {
    Layout::Node const* highlighted_layout_node { nullptr };
    Color tooltip_color;
    Color tooltip_text_color;
    Color tooltip_border_color;
    struct GridHighlight {
        Layout::Node const* layout_node { nullptr };
        GridInspectorOverlayOptions options;
    };
    struct FlexHighlight {
        Layout::Node const* layout_node { nullptr };
        FlexboxInspectorOverlayOptions options;
    };
    Vector<GridHighlight> grid_highlights;
    Vector<FlexHighlight> flex_highlights;
    Optional<CSSPixelRect> caret_debug_rect;
};

WEB_API RefPtr<DisplayList> record_rust_display_list(DOM::Document&, DisplayList const& placeholder_display_list, DisplayListResourceStorage&, PaintCommandCacheMode, HTML::PaintConfig const&, InspectorOverlayInputs const&);
WEB_API Utf16String serialize_painting_dump(DOM::Document const&, AccumulatedVisualContextTree const&, DisplayList const&, DisplayListResourceStorage const&);

WEB_API CSS::ColorResolutionContext gradient_stop_color_resolution_context(Layout::NodeWithStyle const&);
WEB_API DisplayListResource record_image_paint_display_list(ImagePaint const&, ImagePaintRequest const&, double device_pixels_per_css_pixel);

}
