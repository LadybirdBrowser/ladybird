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
#include <LibWeb/Compositor/NavigablePresenter.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/DisplayListRecording.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>
#include <LibWeb/Painting/PaintableTypes.h>

namespace Web::Compositor {

struct FlightPresentation;

}

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

// Starts recording the document's viewport against `visual_context_tree`, unless it has no box to record. The recording
// flies where `blocker` is none, and takes `flight`, if any, to present its frame with beside the event loop.
// Records the document's viewport for the host to publish, or, given `committed`, the presentation of the navigable's
// next frame, commits the frame to the render owner, which presents it beside the event loop.
WEB_API Optional<DisplayListRecording> start_rust_display_list_recording(Layout::BegunRead const&, DOM::Document&, Compositing::AccumulatedVisualContextTree, Optional<Gfx::Color> surface_clear_color, PaintCommandCacheMode, HTML::PaintConfig const&, InspectorOverlayInputs const&, Optional<Compositor::FlightPresentation> committed = {});
// Commits the navigable's next frame, which keeps the display list the compositor has, to the render owner, which
// presents it with `presentation` beside the event loop.
WEB_API void commit_unrecorded_frame(Layout::BegunRead const&, DOM::Document&, Compositor::FlightPresentation presentation);
// Renders the SVG images of the committed frame of `recording` that waits for them, which only the main thread renders,
// into a resource storage of their own, and hands the frame back to the Paint thread, which presents it with them.
WEB_API void render_vector_images(Layout::BegunRead const&, DOM::Document&, DisplayListRecording const& recording);
// Publishes the recording, which has landed and stands, and makes its display list from what was sealed where it began.
WEB_API RefPtr<Compositing::DisplayList> finish_rust_display_list_recording(Layout::BegunRead const&, DOM::Document&, DisplayListRecording const&, Compositing::DisplayListResourceStorage&);
// Hands the document the trace the recording it took in last left, if it left one.
WEB_API void take_recording_trace_if_pending(Layout::BegunRead const&, DOM::Document&);
// Makes the display list of `recording`, published as `presented` says, from what the recording sealed where it began.
// Reads no document, so it runs wherever the recording is published.
WEB_API NonnullRefPtr<Compositing::DisplayList> display_list_of_published_recording(DisplayListRecording const&, Layout::RustFFI::FfiPresentedRecording const&);
WEB_API Utf16String serialize_painting_dump(Layout::BegunRead const&, DOM::Document const&, Compositing::AccumulatedVisualContextTree const&, Compositing::DisplayList const&, Compositing::DisplayListResourceStorage const&);

WEB_API CSS::ColorResolutionContext gradient_stop_color_resolution_context(Layout::NodeWithStyle const&);
// The graph applying a list of filter functions in order, or nothing for an empty list.
WEB_API Optional<Gfx::Filter> filter_from_functions(ReadonlySpan<Compositing::RustFFI::FfiFilterFunction>);

WEB_API Compositing::DisplayListResource record_image_paint_display_list(ImagePaint const&, ImagePaintRequest const&, double device_pixels_per_css_pixel);
// Records one image frame drawn into the destination rectangle, in device pixels.
WEB_API Compositing::DisplayListResource record_image_frame_display_list(Gfx::DecodedImageFrame const&, Gfx::FloatRect const& dest_rect, Gfx::ScalingMode, Compositing::DisplayListResourceStorage&);

}
