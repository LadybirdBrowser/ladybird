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
#include <LibCompositing/Export.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Page/CompositorContextId.h>
#include <LibWebCommon/Page/InputRoutingDecision.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/PixelUnits.h>

namespace Compositing {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ScreenshotRequestId);

enum class WindowResizingInProgress : u8 {
    No,
    Yes,
};

enum class ContextVisibility : u8 {
    Visible,
    Hidden,
};

// Where a reader of the compositor's async scroll updates takes them from: the ones the compositor pushed
// (already here, in order with the input it forwarded), or the compositor's state as of now, asked for
// synchronously, for a reader that routes input against the offsets the compositor holds this instant.
enum class AsyncScrollUpdateFreshness : u8 {
    Pushed,
    FromCompositor,
};

enum class ScrollAnimationKind : u8 {
    SmoothScroll,
    Wheel,
    Momentum,
};

// A scroll the compositor started for user input of its own: the main thread registers it as a user scroll in
// flight. A scroll started for a step of a gesture is owed the scrollend event by that gesture, which continues
// from the offset its steps have asked for; a scroll started for the end of a gesture settles the gesture. The
// selection of a scroll that snapped along no axis carries only the destination. Repeated reports for the same
// operation update its accumulated input without starting another animation.
struct StartedUserScroll {
    Web::AsyncScrollNodeStableID stable_node_id;
    AsyncScrollOperationID operation_id { 0 };
    Web::CSSPixelPoint initial_scroll_offset;
    Web::CSSPixelPoint unsnapped_scroll_destination;
    Web::CSSPixelPoint relative_scroll_delta;
    SnapDestination selection;
    bool settles_gesture { false };
    bool is_absolute_scroll { false };
    Optional<ScrollAnimationKind> animation_kind;
};

// Published with a display list or an incremental scroll-state snapshot. An absent target keeps keyboard default
// actions on WebContent. The epoch also binds incremental updates to the tree whose target they describe.
struct KeyboardScrollState {
    u64 generation { 0 };
    u64 visual_context_tree_structural_epoch { 0 };
    Optional<Web::AsyncScrollNodeStableID> target;
    float page_scroll_distance { 0 };
    float arrow_scroll_distance { 0 };
};

struct PendingAsyncScrollUpdates {
    Optional<Web::UniqueNodeID> document_id;
    // The publication these updates were handed out in, per context and increasing. A scroll state
    // snapshot WebContent produces after adopting them carries it back.
    u64 sequence { 0 };
    Vector<AsyncScrollOffset> scroll_offsets;
    Vector<AsyncScrollOperationID> completed_operation_ids;
    Vector<AsyncScrollOperationID> operation_ids_taken_over_by_user_input;
    Vector<StartedUserScroll> started_user_scrolls;
    bool user_scroll_gesture_in_progress { false };
    bool user_scroll_gesture_ended { false };
};

struct AsyncScrollEnqueueResult {
    bool accepted { false };
    Optional<AsyncScrollOperationID> operation_id {};
    // Why the compositor took or turned away a wheel step, and the scroller it routed the step to.
    Optional<Web::InputRoutingDecision> routing_decision {};
    Optional<Web::AsyncScrollNodeStableID> routed_scroller {};
};

enum class AsyncScrollOperationTracking {
    No,
    Yes,
};

// Blocking wheel event listeners hold a wheel step back from the compositor until the page has dispatched it. A step
// that the page dispatched and did not cancel has nothing left to wait for.
enum class WheelStepDispatch : u8 {
    NotYetDispatched,
    DispatchedAndNotCanceled,
};

// A later keyboard step continues from the destination of a smooth scroll the user started, rather than from its
// presented offset.
enum class SmoothScrollInitiator : u8 {
    Programmatic,
    UserInput,
};

// The main thread allocates the ids of the scrolls it requests from here up, so that they never meet the ids the
// compositor allocates for scrolls of its own.
inline constexpr AsyncScrollOperationID first_operation_id_the_main_thread_requests = AsyncScrollOperationID { 1 } << 63;

// A smooth scroll that the main thread requests, from wherever the box is once the request's instant scroll is done.
struct RequestedSmoothScroll {
    AsyncScrollOperationID operation_id { 0 };
    Gfx::FloatPoint destination_offset;
    ScrollAnimationKind animation_kind { ScrollAnimationKind::SmoothScroll };
    SmoothScrollInitiator initiator { SmoothScrollInitiator::Programmatic };
    // User input that heads the user input scroll of the same operation somewhere else, rather than starting another.
    bool retargets_operation { false };
    // For the scroll that settles a user's gesture, the count of the box's scrolls for the compositor's own user input
    // that the main thread had heard of. User input the compositor took since carries the gesture on instead.
    Optional<u64> settled_user_input_scroll_count;
};

// What the main thread asks of the compositor for one scrolling box, carried with the frame it changed the box in. A
// request aborts the box's smooth scrolls, jumps the box to an offset, then starts a smooth scroll of it, in that
// order. The compositor marks the offsets it reports for the box with the id of the last request it applied to it.
struct ScrollRequest {
    Web::AsyncScrollNodeStableID stable_node_id;
    u64 request_id { 0 };
    Optional<Gfx::FloatPoint> jump_offset;
    // The scale the visual viewport jumps to along with its offset.
    Optional<float> visual_viewport_scale;
    Optional<RequestedSmoothScroll> smooth_scroll;

    // A request made later in the same frame aborts the smooth scroll of this one, which never got under way.
    void fold_later(ScrollRequest const& later)
    {
        request_id = later.request_id;
        if (later.jump_offset.has_value()) {
            jump_offset = later.jump_offset;
            visual_viewport_scale = later.visual_viewport_scale;
        }
        smooth_scroll = later.smooth_scroll;
    }
};

// AD-HOC: Wheel events carry no gesture phase information, so a wheel gesture is considered finished once no input of
//         it has moved a scrolling box for this long. The side that scrolled the gesture's steps decides when its
//         input ran out: the compositor reports the end of a gesture whose steps it chained, and the main thread
//         settles the gesture on that report, or after this delay for the steps it scrolled itself.
inline constexpr AK::Duration user_scroll_settle_delay = AK::Duration::from_milliseconds(500);

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::KeyboardScrollState const&);
template<>
COMPOSITING_API ErrorOr<Compositing::KeyboardScrollState> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::AsyncScrollOffset const&);
template<>
COMPOSITING_API ErrorOr<Compositing::AsyncScrollOffset> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::SnapAreaIdentity const&);
template<>
COMPOSITING_API ErrorOr<Compositing::SnapAreaIdentity> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::SnapDestination const&);
template<>
COMPOSITING_API ErrorOr<Compositing::SnapDestination> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::StartedUserScroll const&);
template<>
COMPOSITING_API ErrorOr<Compositing::StartedUserScroll> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::PendingAsyncScrollUpdates const&);
template<>
COMPOSITING_API ErrorOr<Compositing::PendingAsyncScrollUpdates> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::RequestedSmoothScroll const&);
template<>
COMPOSITING_API ErrorOr<Compositing::RequestedSmoothScroll> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::ScrollRequest const&);
template<>
COMPOSITING_API ErrorOr<Compositing::ScrollRequest> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::AsyncScrollEnqueueResult const&);
template<>
COMPOSITING_API ErrorOr<Compositing::AsyncScrollEnqueueResult> decode(Decoder&);

}
