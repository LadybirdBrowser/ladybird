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
#include <LibCompositing/PageId.h>
#include <LibCompositing/PixelUnits.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibIPC/Forward.h>

namespace Compositing {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CompositorContextId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ScreenshotRequestId);

inline CompositorContextId compositor_context_id_for_page(Compositing::PageId page_id)
{
    VERIFY(page_id.value() > 0);
    return CompositorContextId { page_id.value() };
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

// A scroll the compositor started for user input of its own: the main thread registers it as a user scroll in
// flight. A scroll started for a step of a gesture is owed the scrollend event by that gesture, which continues
// from the offset its steps have asked for; a scroll started for the end of a gesture settles the gesture. The
// selection of a scroll that snapped along no axis carries only the destination. Repeated reports for the same
// operation update its accumulated input without starting another animation.
struct StartedUserScroll {
    AsyncScrollNodeStableID stable_node_id;
    AsyncScrollOperationID operation_id { 0 };
    CSSPixelPoint initial_scroll_offset;
    CSSPixelPoint unsnapped_scroll_destination;
    SnapDestination selection;
    bool settles_gesture { false };
};

// Published with a display list or an incremental scroll-state snapshot. An absent target keeps keyboard default
// actions on WebContent. The epoch also binds incremental updates to the tree whose target they describe.
struct KeyboardScrollState {
    u64 generation { 0 };
    u64 visual_context_tree_structural_epoch { 0 };
    Optional<AsyncScrollNodeStableID> target;
    float page_scroll_distance { 0 };
    float arrow_scroll_distance { 0 };
};

struct PendingAsyncScrollUpdates {
    Optional<UniqueNodeID> document_id;
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
    Optional<AsyncScrollOperationID> operation_id;
};

struct MouseEventHandlingResult {
    bool handled { false };
    // Set when the event belongs to a drag that still has to reach the main thread.
    Optional<ScrollbarDraggedByCompositor> scrollbar_dragged_by_compositor;
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
//         it has moved a scrolling box for this long. The side that scrolled the gesture's steps decides when its
//         input ran out: the compositor reports the end of a gesture whose steps it chained, and the main thread
//         settles the gesture on that report, or after this delay for the steps it scrolled itself.
inline constexpr AK::Duration user_scroll_settle_delay = AK::Duration::from_milliseconds(500);

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::AsyncScrollNodeStableID const&);
template<>
COMPOSITING_API ErrorOr<Compositing::AsyncScrollNodeStableID> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::ScrollbarDraggedByCompositor const&);
template<>
COMPOSITING_API ErrorOr<Compositing::ScrollbarDraggedByCompositor> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::MouseEventHandlingResult const&);
template<>
COMPOSITING_API ErrorOr<Compositing::MouseEventHandlingResult> decode(Decoder&);

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
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::AsyncScrollEnqueueResult const&);
template<>
COMPOSITING_API ErrorOr<Compositing::AsyncScrollEnqueueResult> decode(Decoder&);

}
