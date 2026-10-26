/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/DistinctNumeric.h>
#include <AK/EnumBits.h>
#include <AK/Math.h>
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

// A scroll request the compositor applied, which it tells the main thread of whether or not the box is there.
struct AppliedScrollRequest {
    Web::AsyncScrollNodeStableID stable_node_id;
    u64 request_id { 0 };
    bool moved_the_box { false };
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
    Vector<AppliedScrollRequest> applied_scroll_requests;
    // The boxes whose gesture held input of the compositor's own keeps going, and those whose gesture ended since the
    // previous updates, which may never have been published as going on.
    Vector<Web::AsyncScrollNodeStableID> boxes_in_user_scroll_gesture;
    Vector<Web::AsyncScrollNodeStableID> boxes_whose_user_scroll_gesture_ended;
    Optional<Web::AsyncScrollNodeStableID> wheel_latched_scroller;
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

// An instant scroll by a distance from wherever the box is. Each scroll folded into it clamps the box to the scroll range
// it had, which leaves the box at clamp(offset + delta, lower_bound, upper_bound) in each axis.
struct RequestedRelativeScroll {
    Gfx::FloatPoint delta;
    Gfx::FloatPoint lower_bound { -AK::Infinity<float>, -AK::Infinity<float> };
    Gfx::FloatPoint upper_bound { AK::Infinity<float>, AK::Infinity<float> };

    void then(RequestedRelativeScroll const& later)
    {
        lower_bound = {
            AK::clamp(lower_bound.x() + later.delta.x(), later.lower_bound.x(), later.upper_bound.x()),
            AK::clamp(lower_bound.y() + later.delta.y(), later.lower_bound.y(), later.upper_bound.y()),
        };
        upper_bound = {
            AK::clamp(upper_bound.x() + later.delta.x(), later.lower_bound.x(), later.upper_bound.x()),
            AK::clamp(upper_bound.y() + later.delta.y(), later.lower_bound.y(), later.upper_bound.y()),
        };
        delta.translate_by(later.delta);
    }

    Gfx::FloatPoint applied_to(Gfx::FloatPoint offset) const
    {
        return {
            AK::clamp(offset.x() + delta.x(), lower_bound.x(), upper_bound.x()),
            AK::clamp(offset.y() + delta.y(), lower_bound.y(), upper_bound.y()),
        };
    }
};

// A smooth scroll that the main thread requests, from wherever the box is once the request's instant scroll is done.
struct RequestedSmoothScroll {
    AsyncScrollOperationID operation_id { 0 };
    // Where the scroll goes, or how far it goes from wherever the box is.
    Gfx::FloatPoint destination_offset;
    bool destination_is_relative { false };
    ScrollAnimationKind animation_kind { ScrollAnimationKind::SmoothScroll };
    SmoothScrollInitiator initiator { SmoothScrollInitiator::Programmatic };
    // User input that heads the user input scroll of the same operation somewhere else, rather than starting another.
    bool retargets_operation { false };
    // For the scroll that settles a user's gesture, the count of the box's scrolls for the compositor's own user input
    // that the main thread had heard of. User input the compositor took since carries the gesture on instead.
    Optional<u64> settled_user_input_scroll_count;

    // A later scroll of the same operation goes on from where this one was headed.
    void fold_later(RequestedSmoothScroll const& later)
    {
        VERIFY(later.operation_id == operation_id);
        auto earlier_destination_offset = destination_offset;
        auto earlier_destination_is_relative = destination_is_relative;
        auto earlier_retargets_operation = retargets_operation;
        *this = later;
        retargets_operation |= earlier_retargets_operation;
        if (later.destination_is_relative) {
            destination_offset.translate_by(earlier_destination_offset);
            destination_is_relative = earlier_destination_is_relative;
        }
    }
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
    // A relative scroll after the jump. For the viewport, it can move the visual viewport's position in the page, which
    // the visual viewport takes as much of as it can before the layout viewport takes the rest.
    Optional<RequestedRelativeScroll> relative_scroll;
    bool relative_scroll_moves_visual_viewport_first { false };
    // Whether the instant scroll is the main thread's response to user input, rather than a script's or its own.
    bool instant_scroll_is_for_user_input { false };
    Optional<RequestedSmoothScroll> smooth_scroll;
    // Whether the request scrolls for input that ends the momentum scrolling the box, such as a key.
    bool takes_over_from_momentum { false };

    // A request made later in the same frame aborts the smooth scroll of this one, which never got under way.
    void fold_later(ScrollRequest const& later)
    {
        request_id = later.request_id;
        takes_over_from_momentum |= later.takes_over_from_momentum;
        if (later.jump_offset.has_value() || later.relative_scroll.has_value())
            instant_scroll_is_for_user_input = later.instant_scroll_is_for_user_input;
        if (later.jump_offset.has_value()) {
            jump_offset = later.jump_offset;
            visual_viewport_scale = later.visual_viewport_scale;
            relative_scroll = later.relative_scroll;
            relative_scroll_moves_visual_viewport_first = later.relative_scroll_moves_visual_viewport_first;
        } else if (later.relative_scroll.has_value()) {
            // A box is scrolled by distances of one kind: the viewport by its position in the page, the rest by offset.
            if (relative_scroll.has_value()) {
                VERIFY(relative_scroll_moves_visual_viewport_first == later.relative_scroll_moves_visual_viewport_first);
                relative_scroll->then(later.relative_scroll.value());
            } else {
                relative_scroll = later.relative_scroll;
                relative_scroll_moves_visual_viewport_first = later.relative_scroll_moves_visual_viewport_first;
            }
        }
        if (smooth_scroll.has_value() && later.smooth_scroll.has_value() && smooth_scroll->operation_id == later.smooth_scroll->operation_id)
            smooth_scroll->fold_later(later.smooth_scroll.value());
        else
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
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::AppliedScrollRequest const&);
template<>
COMPOSITING_API ErrorOr<Compositing::AppliedScrollRequest> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::PendingAsyncScrollUpdates const&);
template<>
COMPOSITING_API ErrorOr<Compositing::PendingAsyncScrollUpdates> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::RequestedRelativeScroll const&);
template<>
COMPOSITING_API ErrorOr<Compositing::RequestedRelativeScroll> decode(Decoder&);

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
