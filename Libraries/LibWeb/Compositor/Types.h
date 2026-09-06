/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/DistinctNumeric.h>
#include <AK/EnumBits.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Export.h>

namespace Web::Compositor {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CompositorContextId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ScreenshotRequestId);

inline CompositorContextId compositor_context_id_for_page(u64 page_id)
{
    VERIFY(page_id > 0);
    return CompositorContextId { page_id };
}

enum class WindowResizingInProgress : u8 {
    No,
    Yes,
};

enum class ContextVisibility : u8 {
    Visible,
    Hidden,
};

enum class PagePresentationRegistration {
    No,
    Yes,
};

// Where a reader of the compositor's async scroll updates takes them from: the ones the compositor pushed
// (already here, in order with the input it forwarded), or the compositor's state as of now, asked for
// synchronously, for a reader that routes input against the offsets the compositor holds this instant.
enum class AsyncScrollUpdateFreshness : u8 {
    Pushed,
    FromCompositor,
};

struct PendingAsyncScrollUpdates {
    // The publication these updates were handed out in, per context and increasing. A scroll state
    // snapshot WebContent produces after adopting them carries it back.
    u64 sequence { 0 };
    Vector<AsyncScrollOffset> scroll_offsets;
    Vector<AsyncScrollOperationID> completed_operation_ids;
    Vector<AsyncScrollOperationID> operation_ids_taken_over_by_user_input;
    bool user_scroll_gesture_in_progress { false };
    bool user_scroll_gesture_ended { false };
};

struct AsyncScrollEnqueueResult {
    bool accepted { false };
    Optional<AsyncScrollOperationID> operation_id;
};

enum class AsyncScrollOperationTracking {
    No,
    Yes,
};

enum class ScrollAnimationKind : u8 {
    SmoothScroll,
    Momentum,
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::AsyncScrollNodeStableID const&);
template<>
WEB_API ErrorOr<Web::Compositor::AsyncScrollNodeStableID> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::AsyncScrollOffset const&);
template<>
WEB_API ErrorOr<Web::Compositor::AsyncScrollOffset> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::PendingAsyncScrollUpdates const&);
template<>
WEB_API ErrorOr<Web::Compositor::PendingAsyncScrollUpdates> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::AsyncScrollEnqueueResult const&);
template<>
WEB_API ErrorOr<Web::Compositor::AsyncScrollEnqueueResult> decode(Decoder&);

}
