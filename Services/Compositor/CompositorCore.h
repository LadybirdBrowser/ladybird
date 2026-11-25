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
#include <LibCompositing/Types.h>
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
    using PresentationCapabilities = RustFFI::FfiPresentationCapabilities;

    void create_context(Web::CompositorContextId, Optional<u64> page_id, WebContentClientId);
    // Also leaves the contexts nested in it without a parent.
    void destroy_context(Web::CompositorContextId);
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, WebContentClientId) const;
    bool context_is_effectively_visible(Web::CompositorContextId) const;
    Optional<Web::CompositorContextId> parent_context(Web::CompositorContextId) const;
    Optional<PresentationId> bound_presentation(Web::CompositorContextId) const;
    // Ends the fling of a context that is hidden, whether or not it was visible. Answers whether the context became
    // visible.
    bool set_visibility(Web::CompositorContextId, Compositing::ContextVisibility);

    void register_presentation(PresentationId, PresentationCapabilities const&);
    // Leaves every context bound to the presentation unbound.
    void unregister_presentation(PresentationId);

    void presented_store_released(Web::CompositorContextId, i32 bitmap_id);
    void vsync(Optional<u64> display_id, MonotonicTime frame_time);
    void gpu_work_finished(u64 token);
    void frame_ready_for_hand_off(u64 token);
    // A video sink or an image frame that the context draws has new pixels.
    void context_content_changed(Web::CompositorContextId);
    void canvas_content_changed(WebContentClientId, Compositing::CanvasId);
    void forget_web_content_client(WebContentClientId);

    // Answers false for a context with nothing to paint yet.
    bool paint_screenshot(Web::CompositorContextId, Gfx::PaintingSurface&);
    struct PresentedFrame {
        NonnullRefPtr<Gfx::PaintingSurface> surface;
        Gfx::IntSize frame_size;
    };
    // The surface that holds the frame on screen as a whole, and the size of the frame in it.
    Optional<PresentedFrame> presented_frame_for_snapshot(Web::CompositorContextId);

    void timer_fired(u64 timer_number);

    // The messages of CompositorWebContentServer, CompositorPresentationServer, CompositorControlServer and
    // RenderClockServer that are marked [RustReceiver].
    IPC::RawMessage::OutcomeInRust handle_message_from_web_content(WebContentClientId, IPC::RawMessage&);
    IPC::RawMessage::OutcomeInRust handle_message_from_presentation(PresentationId, IPC::RawMessage&);
    IPC::RawMessage::OutcomeInRust handle_message_from_control(IPC::RawMessage&);
    IPC::RawMessage::OutcomeInRust handle_message_from_render_clock(WebContentClientId, IPC::RawMessage&);

private:
    RustFFI::CompositorCore* m_core { nullptr };
};

}
