/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Time.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Compositor {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, WebContentClientId);

enum class ContextTimer : u8 {
    ScrollStepGestureInput,
    ScrollbarPaging,
    ScrollbarFadeOut,
    CaretBlink,
};

// What the scroll and input logic of the contexts asks of the process around it. None of these calls comes back into
// that logic before it returns.
class CompositorCoreHost {
public:
    virtual ~CompositorCoreHost() = default;

    // A message for a client that is gone is dropped.
    virtual void post_message_to_web_content(WebContentClientId, IPC::MessageBuffer&) = 0;
    // A client without a render clock channel hears nothing.
    virtual void post_message_to_render_clock_of_web_content(WebContentClientId, IPC::MessageBuffer&) = 0;
    // Display ticks reach a context whose scroll and input logic says it needs animation frames, while it is visible.
    virtual void request_display_ticks(Web::CompositorContextId) = 0;
    // Each timer of a context fires once after the delay, and setting it again replaces the delay.
    virtual void set_timer(Web::CompositorContextId, ContextTimer, AK::Duration delay) = 0;
    virtual void cancel_timer(Web::CompositorContextId, ContextTimer) = 0;
};

}
