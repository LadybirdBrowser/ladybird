/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <Compositor/CompositorCoreHost.h>
#include <Compositor/PaintingSurface.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/Types.h>
#include <LibCompositing/UIOverlays.h>
#include <LibIPC/Encoded.h>
#include <LibIPC/Message.h>
#include <LibIPC/RawMessage.h>

namespace Compositor {

// The logic of the compositor, which is Rust (compositor_core): the contexts, their scrolling and input, and when and
// what of their frames is painted and presented. It talks to the process through its hosts, which paint and present as
// it asks, and it is never entered while one of its own calls runs.
class CompositorCore {
    AK_MAKE_NONCOPYABLE(CompositorCore);
    AK_MAKE_NONMOVABLE(CompositorCore);

public:
    CompositorCore(CompositorCoreHost&, RustFFI::FfiRasterHost, bool scrollbars_always_shown, RustFFI::GpuBackend);
    ~CompositorCore();

    using ContextOwnerCheckResult = RustFFI::ContextOwnerCheckResult;
    using VisualContextTreeUpdateResult = RustFFI::VisualContextTreeUpdateResult;
    using PresentationCapabilities = RustFFI::FfiPresentationCapabilities;

    void create_context(Web::CompositorContextId, Optional<u64> page_id, WebContentClientId);
    // Also leaves the contexts nested in it without a parent.
    void destroy_context(Web::CompositorContextId);
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, WebContentClientId) const;
    bool context_can_be_nested_in(Web::CompositorContextId, Web::CompositorContextId parent_context_id) const;
    bool presents_to_client(Web::CompositorContextId) const;
    bool context_is_effectively_visible(Web::CompositorContextId) const;
    Optional<Web::CompositorContextId> parent_context(Web::CompositorContextId) const;
    Optional<PresentationId> bound_presentation(Web::CompositorContextId) const;
    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>);
    void stop_presenting_to_client(Web::CompositorContextId);
    // Ends the fling of a context that is hidden, whether or not it was visible. Answers whether the context became
    // visible.
    bool set_visibility(Web::CompositorContextId, Compositing::ContextVisibility);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize, double device_pixel_ratio, Compositing::WindowResizingInProgress);
    void set_ui_overlays(Web::CompositorContextId, Compositing::UIOverlays const&);

    void register_presentation(PresentationId, PresentationCapabilities const&);
    // Leaves every context bound to the presentation unbound.
    void unregister_presentation(PresentationId);
    void set_presentation_gpu_capability(PresentationId, bool supported, u64 adapter_luid);
    void set_scrollbar_track_press_jumps(PresentationId, bool jumps);
    void bind_presentation(Web::CompositorContextId, PresentationId);
    void set_shown_in_remote_layer(Web::CompositorContextId, bool shown);
    void set_display_metadata(Web::CompositorContextId, Optional<u64> display_id, double refresh_rate);
    void set_window_server_may_show_high_dynamic_range(bool);
    void set_presents_from_cpu_memory(Web::CompositorContextId, bool);

    void request_rendering_opportunity(Web::CompositorContextId, double maximum_frames_per_second);
    void request_clock_tick(Web::CompositorContextId, double maximum_frames_per_second);
    void hurry_rendering_opportunity(Web::CompositorContextId);
    void present_frame_of_page(Web::CompositorContextId, Gfx::IntRect viewport_rect);
    void presented_store_released(Web::CompositorContextId, i32 bitmap_id);
    void vsync(Optional<u64> display_id, MonotonicTime frame_time);
    void gpu_work_finished(u64 token);
    void frame_ready_for_hand_off(u64 token);
    // A video sink or an image frame that the context draws has new pixels.
    void context_content_changed(Web::CompositorContextId);
    void canvas_content_changed(WebContentClientId, Compositing::CanvasId);

    // Answers false for a context with nothing to paint yet.
    bool paint_screenshot(Web::CompositorContextId, Gfx::PaintingSurface&);
    struct PresentedFrame {
        NonnullRefPtr<Gfx::PaintingSurface> surface;
        Gfx::IntSize frame_size;
    };
    // The surface that holds the frame on screen as a whole, and the size of the frame in it.
    Optional<PresentedFrame> presented_frame_for_snapshot(Web::CompositorContextId);

    // Answers false, changing nothing, for async scrolling metadata or a scroll state that do not decode.
    bool install_display_list_update(Web::CompositorContextId, Compositing::DisplayList const&, IPC::Encoded<Optional<Compositing::DisplayList::AsyncScrollingMetadata>> const&, Compositing::AccumulatedVisualContextTree const&, ReadonlyBytes encoded_scroll_state);
    // Changes nothing for a scroll state or keyboard scroll state that do not decode. Of a tree that does not fit the
    // installed display list, only the scroll state is taken.
    VisualContextTreeUpdateResult update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree const&, ReadonlyBytes encoded_scroll_state, IPC::Encoded<Compositing::KeyboardScrollState> const&);

    void timer_fired(Web::CompositorContextId, ContextTimer);

    // The messages of CompositorWebContentServer and CompositorPresentationServer that are marked [RustReceiver].
    IPC::RawMessage::OutcomeInRust handle_message_from_web_content(WebContentClientId, IPC::RawMessage&);
    IPC::RawMessage::OutcomeInRust handle_message_from_presentation(PresentationId, IPC::RawMessage&);

private:
    RustFFI::CompositorCore* m_core { nullptr };
};

}
