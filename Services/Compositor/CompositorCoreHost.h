/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <Compositor/CompositorCoreRustFFI.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Compositor {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, WebContentClientId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, PresentationId);

struct PendingFrame {
    Gfx::IntRect viewport_rect;
    Gfx::IntRect forced_damage_rect;

    static PendingFrame repainting_everything(Gfx::IntRect viewport_rect) { return { viewport_rect, { {}, viewport_rect.size() } }; }
    static PendingFrame repainting_changes(Gfx::IntRect viewport_rect) { return { viewport_rect, {} }; }
};

using PresentScheduling = RustFFI::PresentScheduling;

// A frame that the scroll and input logic of a context wants presented. The compositor schedules it once the call that
// asked for it has returned.
struct PresentRequest {
    Web::CompositorContextId context_id;
    // Empty for a frame at the viewport rect the context is presented at.
    Optional<Gfx::IntRect> viewport_rect;
    Gfx::IntRect forced_damage_rect;
    PresentScheduling scheduling { PresentScheduling::OnNextDisplayTick };
};

// What the presentation knows of a context as a tick of a display begins.
struct DisplayTickInputs {
    bool context_is_on_tick_display { false };
    bool present_is_scheduled_on_tick_display { false };
    bool ui_overlay_fades_need_frame { false };
    // Set when the render clock of the context is due a tick now.
    Optional<double> clock_tick_interval_in_milliseconds;
};

using DisplayTickResult = RustFFI::DisplayTickResult;

using ContextTimer = RustFFI::ContextTimer;

// What the scroll and input logic of the contexts asks of the process around it. None of these calls comes back into
// that logic before it returns.
class CompositorCoreHost {
public:
    virtual ~CompositorCoreHost() = default;

    // A message for a client that is gone is dropped.
    virtual void post_message_to_web_content(WebContentClientId, IPC::MessageBuffer&) = 0;
    // A client without a render clock channel hears nothing.
    virtual void post_message_to_render_clock_of_web_content(WebContentClientId, IPC::MessageBuffer&) = 0;
    // A message for a presentation that is gone is dropped.
    virtual void post_message_to_presentation(PresentationId, IPC::MessageBuffer&) = 0;
    // Display ticks reach a context whose scroll and input logic says it needs animation frames, while it is visible.
    virtual void request_display_ticks(Web::CompositorContextId) = 0;
    // Each timer of a context fires once after the delay, and setting it again replaces the delay.
    virtual void set_timer(Web::CompositorContextId, ContextTimer, AK::Duration delay) = 0;
    virtual void cancel_timer(Web::CompositorContextId, ContextTimer) = 0;
};

}
