/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::Painting {

struct BlockingWheelEventRegionState {
    bool has_blocking_wheel_event_listeners { false };
    bool has_blocking_wheel_event_region_covering_viewport { false };
};

class WEB_API DocumentPaintState {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DocumentPaintState(Layout::NodeArena&);

    void viewport_row_was_reset();

    BlockingWheelEventRegionState collect_root_blocking_wheel_event_regions(DOM::Document&);

    // The mapping through the document's visual context tree, brought up to date, with the compositor's animations
    // sampled at the time where one is given. It reads the scroll state, and the tree the host keeps for its mappings,
    // where it is used.
    Layout::RustFFI::FfiViewportMapping viewport_mapping(DOM::Document const&, Optional<MonotonicTime> visual_animation_sample_time, double device_pixels_per_css_pixel) const;
    // Whether the scroll translations of the space the box is painted in, and the origins drawn in it, round to whole
    // device pixels: no transform but identities lies above it in the visual context tree, brought up to date.
    bool translations_snap_to_device_pixels(DOM::Document const&, Compositing::RustFFI::NodeSlotId box) const;
    // Only the scroll logic of the compositor, which is Rust, decodes it.
    ByteBuffer encoded_scroll_state() const;
    void did_update_visual_context_values();

    void update_visual_viewport_accumulated_visual_context(Layout::BegunRead const& read, DOM::Document&);
    // An update pass publishes each effect's compositor animations into a list the tree takes at the end.
    void begin_compositor_animation_update();
    enum class PublishPendingCompositorAnimations {
        No,
        Yes,
    };
    void publish_compositor_animations(Layout::BegunRead const& read, PublishPendingCompositorAnimations);
    void republish_visual_animations(Layout::BegunRead const& read);
    // Whether the visual context tree changed since a frame last took it to the compositor, which the caller's frame now
    // does.
    bool take_visual_context_tree_compositor_update(Layout::BegunRead const& read);
    bool has_visual_context_tree(Layout::BegunRead const& read) const;
    void append_recording_trace(String trace) { m_recording_traces.append(move(trace)); }
    Vector<String> take_recording_traces() { return exchange(m_recording_traces, {}); }

    void recompute_selection_states(Layout::BegunRead const& read, DOM::Document&, DOM::Range&);
    void reset_selection_states(Layout::BegunRead const& read, DOM::Document&);
    void recompute_search_text_states(Layout::BegunRead const& read, DOM::Document&, GC::Ptr<DOM::Range> active_match, Vector<GC::Ref<DOM::Range>> const& highlighted_matches);
    void reset_search_text_states();
    void recompute_spelling_error_states(Layout::BegunRead const& read, DOM::Document&, Editing::SpellingMarkers&);
    void reset_spelling_error_states();
    void recompute_custom_highlight_states(Layout::BegunRead const& read, DOM::Document&, CSS::HighlightRegistry&);
    void reset_custom_highlight_states();

    void set_boxes_with_auto_content_visibility(Vector<Compositing::RustFFI::NodeSlotId> boxes) { m_boxes_with_auto_content_visibility = move(boxes); }
    Vector<Compositing::RustFFI::NodeSlotId> const& boxes_with_auto_content_visibility() const { return m_boxes_with_auto_content_visibility; }

    Compositing::AccumulatedVisualContextTree visual_context_tree(DOM::Document const&) const;
    u64 visual_context_tree_structural_epoch(Layout::BegunRead const& read) const;

    void set_display_list_used_as_paint_command_cache_source(RefPtr<Compositing::DisplayList> display_list, Compositing::DisplayListResourceSet referenced_resources)
    {
        m_display_list_used_as_paint_command_cache_source = move(display_list);
        m_paint_command_cache_source_referenced_resources = move(referenced_resources);
    }
    Compositing::DisplayList* display_list_used_as_paint_command_cache_source() const { return m_display_list_used_as_paint_command_cache_source.ptr(); }
    Compositing::DisplayListResourceSet const& paint_command_cache_source_referenced_resources() const { return m_paint_command_cache_source_referenced_resources; }

    void append_paint_command_cache_source_resources(Compositing::DisplayListResourceSet&) const;

private:
    Vector<String> m_recording_traces;

    NonnullRefPtr<Layout::NodeArena> m_layout_node_arena;

    Vector<Compositing::RustFFI::NodeSlotId> m_boxes_with_auto_content_visibility;

    RefPtr<Compositing::DisplayList> m_display_list_used_as_paint_command_cache_source;
    Compositing::DisplayListResourceSet m_paint_command_cache_source_referenced_resources;
};

}
