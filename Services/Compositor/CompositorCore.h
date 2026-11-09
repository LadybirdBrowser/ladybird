/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <Compositor/CompositorCoreHost.h>
#include <Compositor/FrameInputs.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/Types.h>
#include <LibIPC/Encoded.h>
#include <LibIPC/Message.h>
#include <LibIPC/RawMessage.h>

namespace Compositor {

// The scroll and input logic of the contexts and the routing of input between them, which is Rust (compositor_core).
// It talks to the process through its host, hands the frames it wants presented back as requests, and is never
// entered while one of its own calls runs.
class CompositorCore {
    AK_MAKE_NONCOPYABLE(CompositorCore);
    AK_MAKE_NONMOVABLE(CompositorCore);

public:
    CompositorCore(CompositorCoreHost&, bool scrollbars_always_shown);
    ~CompositorCore();

    using ContextOwnerCheckResult = RustFFI::ContextOwnerCheckResult;
    using VisualContextTreeUpdateResult = RustFFI::VisualContextTreeUpdateResult;
    // What the presentation a context is shown through makes of the user's scrolling.
    using ScrollCapabilities = RustFFI::FfiScrollCapabilities;

    void create_context(Web::CompositorContextId, Optional<u64> page_id, WebContentClientId);
    // Also leaves the contexts nested in it without a parent.
    void destroy_context(Web::CompositorContextId);
    ContextOwnerCheckResult check_context_owner(Web::CompositorContextId, WebContentClientId) const;
    void set_parent_context(Web::CompositorContextId, Optional<Web::CompositorContextId>);
    void stop_presenting_to_client(Web::CompositorContextId);
    // Ends the fling of a context that is hidden, whether or not it was visible.
    void set_visibility(Web::CompositorContextId, Compositing::ContextVisibility, MonotonicTime now);
    void viewport_size_updated(Web::CompositorContextId, Gfx::IntSize);
    void set_paused_debugger_overlay_is_shown(Web::CompositorContextId, bool);
    void set_bound_presentation(Web::CompositorContextId, Optional<PresentationId>);
    void set_scroll_capabilities(Web::CompositorContextId, ScrollCapabilities const&);

    // Answers false, changing nothing, for async scrolling metadata or a scroll state that do not decode.
    bool install_display_list_update(Web::CompositorContextId, Compositing::DisplayList const&, IPC::Encoded<Optional<Compositing::DisplayList::AsyncScrollingMetadata>> const&, Compositing::AccumulatedVisualContextTree const&, ReadonlyBytes encoded_scroll_state, MonotonicTime now);
    // Changes nothing for a scroll state or keyboard scroll state that do not decode. Of a tree that does not fit the
    // installed display list, only the scroll state is taken.
    VisualContextTreeUpdateResult update_visual_context_tree(Web::CompositorContextId, Compositing::AccumulatedVisualContextTree const&, ReadonlyBytes encoded_scroll_state, IPC::Encoded<Compositing::KeyboardScrollState> const&, MonotonicTime now);

    bool needs_animation_frames(Web::CompositorContextId, MonotonicTime now);
    // Each frame shows the scrollbars as of when it is painted, so this updates which of them are shown. Empty until a
    // display list is installed.
    Optional<FrameInputs> frame_inputs(Web::CompositorContextId, MonotonicTime now);
    struct ScreenshotInputs {
        FrameInputs frame_inputs;
        // The scrollbar fade damage that the screenshot does not present on screen.
        Gfx::IntRect scrollbar_fade_damage_rect;
    };
    ScreenshotInputs frame_inputs_for_screenshot(Web::CompositorContextId, MonotonicTime now);
    DisplayTickResult display_tick(Web::CompositorContextId, MonotonicTime frame_time, MonotonicTime now, DisplayTickInputs const&);
    void timer_fired(Web::CompositorContextId, ContextTimer, MonotonicTime now);

    // The frames asked for since the last call, in the order they were asked for.
    Vector<PresentRequest> take_present_requests();

    // The messages of CompositorWebContentServer and CompositorPresentationServer that are marked [RustReceiver].
    IPC::RawMessage::OutcomeInRust handle_message_from_web_content(WebContentClientId, IPC::RawMessage&, MonotonicTime now);
    IPC::RawMessage::OutcomeInRust handle_message_from_presentation(PresentationId, IPC::RawMessage&, MonotonicTime now);

private:
    RustFFI::CompositorCore* m_core { nullptr };
};

}
