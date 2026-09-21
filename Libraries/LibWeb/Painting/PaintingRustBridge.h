/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Types.h>
#include <LibGfx/Filter.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Compositor/VisualAnimation.h>
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
WEB_API void dump_layout_tree(StringBuilder&, Layout::Node const&, bool interactive);

WEB_API Layout::RustFFI::FfiVisualContextUpdateOutcome rust_update_accumulated_visual_contexts(DOM::Document&);
WEB_API Vector<u32> rust_owned_visual_context_node_indices(Layout::Node const&, Layout::RustFFI::FfiVisualContextBoxNodeList);
WEB_API Vector<u32> rust_visual_animation_target_node_indices(Layout::Node const&, AccumulatedVisualContextTree const&, Layout::RustFFI::FfiVisualAnimationTargetKind);
WEB_API bool rust_background_color_can_be_compositor_animated(Layout::Node const&);
WEB_API void const* retain_rust_main_visual_context_tree(DOM::Document const&);
WEB_API Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Layout::Node const&);
WEB_API void register_geometry_host(Layout::NodeArena&);
WEB_API Layout::RustFFI::FfiRenderingPreparationOutcome rust_prepare_for_rendering(DOM::Document&, bool visual_context_update_pending);
WEB_API void rust_update_visual_viewport_transform(DOM::Document&);
enum class ForceScrollStateRefresh {
    No,
    Yes,
};
// Refreshes the snapshot from the Rust scroll state; false when nothing had invalidated it and
// the refresh was not forced.
WEB_API bool rust_refresh_scroll_state(DOM::Document&, ScrollStateSnapshot&, ForceScrollStateRefresh = ForceScrollStateRefresh::No);
WEB_API void rust_invalidate_scroll_state(DOM::Document&);
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
// The graph applying a list of filter functions in order, or nothing for an empty list.
WEB_API Optional<Gfx::Filter> filter_from_functions(ReadonlySpan<Layout::RustFFI::FfiFilterFunction>);

// The descriptors the visual context tree takes a list of animations over as. They borrow the animations' node
// indices and transform values, so the animations must outlive them.
class WEB_API VisualAnimationFfiDescriptors {
    AK_MAKE_NONCOPYABLE(VisualAnimationFfiDescriptors);
    AK_MAKE_NONMOVABLE(VisualAnimationFfiDescriptors);

public:
    explicit VisualAnimationFfiDescriptors(ReadonlySpan<Compositor::VisualAnimation>);

    ReadonlySpan<Layout::RustFFI::FfiVisualAnimation> descriptors() const { return m_animations; }

private:
    Layout::RustFFI::FfiEasingDescriptor easing_descriptor(Compositor::VisualAnimationEasing const&);
    Layout::RustFFI::FfiVisualAnimationKeyframe keyframe_descriptor(Compositor::VisualAnimationKeyframe const&);

    // Each vector is sized for everything it will hold before any pointer into it is taken.
    Vector<Layout::RustFFI::FfiLinearEasingPoint> m_linear_points;
    Vector<Layout::RustFFI::FfiFilterFunction> m_filter_functions;
    Vector<Layout::RustFFI::FfiVisualAnimationTransformOperation> m_transform_operations;
    Vector<Layout::RustFFI::FfiVisualAnimationKeyframe> m_keyframes;
    Vector<Layout::RustFFI::FfiVisualAnimation> m_animations;
};

WEB_API DisplayListResource record_image_paint_display_list(ImagePaint const&, ImagePaintRequest const&, double device_pixels_per_css_pixel);

}
