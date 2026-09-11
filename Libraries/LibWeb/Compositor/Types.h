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
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Export.h>
#include <LibWeb/PixelUnits.h>

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

// A snap scroll the compositor started for a wheel step of its own: the main thread registers it as a user scroll
// in flight, so that the gesture the step belongs to owes the scrollend event and continues from the offset its
// steps have asked for.
struct StartedSnapScroll {
    AsyncScrollNodeStableID stable_node_id;
    AsyncScrollOperationID operation_id { 0 };
    CSSPixelPoint initial_scroll_offset;
    CSSPixelPoint destination_scroll_offset;
    CSSPixelPoint unsnapped_scroll_destination;
    bool evaluated_x { false };
    bool evaluated_y { false };
    Vector<SnapAreaIdentity> snapped_areas_x;
    Vector<SnapAreaIdentity> snapped_areas_y;
};

struct PendingAsyncScrollUpdates {
    // The publication these updates were handed out in, per context and increasing. A scroll state
    // snapshot WebContent produces after adopting them carries it back.
    u64 sequence { 0 };
    Vector<AsyncScrollOffset> scroll_offsets;
    Vector<AsyncScrollOperationID> completed_operation_ids;
    Vector<AsyncScrollOperationID> operation_ids_taken_over_by_user_input;
    Vector<StartedSnapScroll> started_snap_scrolls;
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

// AD-HOC: Wheel events carry no gesture phase information, so a wheel gesture is considered finished once no input of
//         it has moved a scrolling box for this long. The main thread settles the gesture after this delay, and the
//         compositor chains the steps of a gesture across it.
inline constexpr AK::Duration user_scroll_settle_delay = AK::Duration::from_milliseconds(500);

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
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::SnapAreaIdentity const&);
template<>
WEB_API ErrorOr<Web::Compositor::SnapAreaIdentity> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::StartedSnapScroll const&);
template<>
WEB_API ErrorOr<Web::Compositor::StartedSnapScroll> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::PendingAsyncScrollUpdates const&);
template<>
WEB_API ErrorOr<Web::Compositor::PendingAsyncScrollUpdates> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::AsyncScrollEnqueueResult const&);
template<>
WEB_API ErrorOr<Web::Compositor::AsyncScrollEnqueueResult> decode(Decoder&);

}
