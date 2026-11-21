/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Compositor/NavigablePresenter.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/DisplayListDumpOptions.h>
#include <LibWeb/Painting/DisplayListRecording.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>
#include <LibWeb/Painting/PaintableTypes.h>

namespace Web::Compositor {

class CompositorHost;
struct FlightPresentation;

}

namespace Web::Painting {

struct ImagePaint;
struct ImagePaintRequest;

WEB_API void dump_stacking_context_tree(Layout::BegunRead const&, StringBuilder&, DOM::Document const&);
WEB_API void dump_layout_tree(StringBuilder&, Layout::Node const&, bool interactive);

WEB_API Compositing::RustFFI::FfiVisualContextTreeInputs visual_context_tree_inputs(DOM::Document&);
WEB_API bool rust_background_color_can_be_compositor_animated(Layout::Node const&);
WEB_API Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Layout::Node const&);
WEB_API void register_geometry_host(Layout::NodeArena&);
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

// Starts recording the document's viewport against its visual context tree, of the structural epoch given, unless it has
// no box to record. Records the document's viewport for the host to publish, or, given `committed`, the presentation of
// the navigable's next frame, commits the frame to the render owner, which presents it beside the event loop.
WEB_API Optional<DisplayListRecording> start_rust_display_list_recording(Layout::BegunRead const&, DOM::Document&, u64 visual_context_tree_structural_epoch, Optional<Gfx::Color> surface_clear_color, PaintCommandCacheMode, HTML::PaintConfig const&, InspectorOverlayInputs const&, Optional<Compositor::FlightPresentation> committed = {});
// Commits the navigable's next frame, which keeps the display list the compositor has, to the render owner, which
// presents it with `presentation` beside the event loop.
WEB_API void commit_unrecorded_frame(Layout::BegunRead const&, DOM::Document&, Compositor::FlightPresentation presentation);
// Renders the SVG images of the document's committed frame that waits for them, which only the main thread renders, and
// hands the frame back to the Paint thread, which presents it with them.
WEB_API void render_vector_images(Layout::BegunRead const&, DOM::Document&);
// Publishes the recording, which has landed and stands.
WEB_API void publish_rust_display_list_recording(Layout::BegunRead const&, DOM::Document&);
// Hands the document the trace the recording it took in last left, if it left one.
WEB_API void take_recording_trace_if_pending(Layout::BegunRead const&, DOM::Document&);
// Dumps the display list the document recorded last.
WEB_API Utf16String serialize_painting_dump(Layout::BegunRead const&, DOM::Document const&, DisplayListDumpOptions const&);
WEB_API Utf16String display_list_dump_changes(Optional<StringView> previous_jsonl, StringView jsonl);

struct OwnersOfVisualContextNodes {
    // Zero for a document without a visual context tree.
    u64 structural_epoch { 0 };
    // By the position of each node among the spatial nodes and then the effect nodes asked of, empty for a node without
    // a live owner.
    Vector<String> owners;
};
// Describes the boxes that own the nodes of the document's visual context tree, as the boxes describe themselves.
WEB_API OwnersOfVisualContextNodes describe_owners_of_visual_context_nodes(DOM::Document const&, ReadonlySpan<u32> spatial_node_indices, ReadonlySpan<u32> effect_node_indices);

WEB_API Layout::RustFFI::FfiVectorImageKey vector_image_key_to_ffi(VectorImageKey const&);
// Has the compositor rasterize `paint` into `target` as `request` asks, which must be a shareable BGRA8888 bitmap with
// premultiplied alpha. Returns false if nothing was drawn.
WEB_API bool rasterize_image_paint(Compositor::CompositorHost const&, ImagePaint const&, ImagePaintRequest const&, double device_pixels_per_css_pixel, NonnullRefPtr<Gfx::Bitmap> target);
// Has the compositor rasterize one image frame drawn into the destination rectangle, in device pixels, into `target`.
WEB_API bool rasterize_image_frame(Compositor::CompositorHost const&, Gfx::DecodedImageFrame const&, Gfx::FloatRect const& dest_rect, Gfx::ScalingMode, NonnullRefPtr<Gfx::Bitmap> target);

}
