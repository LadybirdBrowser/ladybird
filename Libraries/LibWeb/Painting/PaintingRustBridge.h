/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/Filter.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>
#include <LibWeb/Painting/PaintableTypes.h>

namespace Web::Painting {

struct ImagePaint;
struct ImagePaintRequest;

WEB_API void dump_stacking_context_tree(Layout::BegunRead const&, StringBuilder&, DOM::Document const&);
WEB_API void dump_layout_tree(StringBuilder&, Layout::Node const&, bool interactive);

WEB_API Layout::RustFFI::FfiVisualContextUpdateOutcome rust_update_accumulated_visual_contexts(Layout::BegunRead const&, DOM::Document&);
WEB_API Vector<u32> rust_owned_visual_context_node_indices(Layout::Node const&, Layout::RustFFI::FfiVisualContextBoxNodeList);
WEB_API bool rust_background_color_can_be_compositor_animated(Layout::Node const&);
WEB_API void const* retain_rust_main_visual_context_tree(Layout::BegunRead const&, DOM::Document const&);
WEB_API Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Layout::Node const&);
WEB_API void register_geometry_host(Layout::NodeArena&);
WEB_API Layout::RustFFI::FfiRenderingPreparationOutcome rust_prepare_for_rendering(Layout::BegunRead const&, DOM::Document&, bool visual_context_update_pending);
WEB_API void rust_update_visual_viewport_transform(Layout::BegunRead const&, DOM::Document&);
enum class ForceScrollStateRefresh {
    No,
    Yes,
};
// Refreshes the snapshot from the Rust scroll state; false when nothing had invalidated it and
// the refresh was not forced.
WEB_API bool rust_refresh_scroll_state(Layout::BegunRead const&, DOM::Document&, Compositing::ScrollStateSnapshot&, ForceScrollStateRefresh = ForceScrollStateRefresh::No);
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

// A recording of a document's display list, started from the document as it was then: done in step with the host, or
// in flight beside the event loop until the event loop takes it in. Its display list is made once it has landed.
struct DisplayListRecording {
    Compositing::AccumulatedVisualContextTree visual_context_tree;
    NonnullRefPtr<Compositing::DisplayList> placeholder_display_list;
    PaintCommandCacheMode cache_mode;
    bool in_flight { false };
    DevicePixelRect device_viewport_rect;
    BlockingWheelEventRegionState wheel_event_region_state;
};

// Starts recording the document's viewport against `visual_context_tree`, unless it has no box to record. The recording
// flies where `blocker` is none.
WEB_API Optional<DisplayListRecording> start_rust_display_list_recording(Layout::BegunRead const&, DOM::Document&, Compositing::AccumulatedVisualContextTree, NonnullRefPtr<Compositing::DisplayList> placeholder_display_list, PaintCommandCacheMode, HTML::PaintConfig const&, InspectorOverlayInputs const&, Layout::RustFFI::FfiFlightBlocker);
// Publishes the recording, which has landed and stands, and makes its display list.
WEB_API RefPtr<Compositing::DisplayList> finish_rust_display_list_recording(Layout::BegunRead const&, DOM::Document&, DisplayListRecording const&, Compositing::DisplayListResourceStorage&);
WEB_API Utf16String serialize_painting_dump(Layout::BegunRead const&, DOM::Document const&, Compositing::AccumulatedVisualContextTree const&, Compositing::DisplayList const&, Compositing::DisplayListResourceStorage const&);

WEB_API CSS::ColorResolutionContext gradient_stop_color_resolution_context(Layout::NodeWithStyle const&);
// The graph applying a list of filter functions in order, or nothing for an empty list.
WEB_API Optional<Gfx::Filter> filter_from_functions(ReadonlySpan<Compositing::RustFFI::FfiFilterFunction>);

WEB_API Compositing::DisplayListResource record_image_paint_display_list(ImagePaint const&, ImagePaintRequest const&, double device_pixels_per_css_pixel);

}
