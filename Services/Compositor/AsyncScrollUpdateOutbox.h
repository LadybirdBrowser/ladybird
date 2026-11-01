/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Types.h>

namespace Compositor {

class AsyncScrollUpdateOutbox {
public:
    Compositing::AsyncScrollOperationID allocate_operation_id() { return ++m_last_allocated_operation_id; }
    void complete_operation(Compositing::AsyncScrollOperationID operation_id) { m_completed_operation_ids.append(operation_id); }
    void note_operation_taken_over_by_user_input(Compositing::AsyncScrollOperationID operation_id) { m_operation_ids_taken_over_by_user_input.append(operation_id); }
    void record_scroll_offsets(ReadonlySpan<Compositing::AsyncScrollOffset>);
    void record_started_user_scroll(Compositing::StartedUserScroll started_user_scroll) { m_started_user_scrolls.append(move(started_user_scroll)); }
    void note_user_scroll_gesture_ended(ReadonlySpan<Web::AsyncScrollNodeStableID>);
    void note_scroll_request_applied(Compositing::AppliedScrollRequest applied_scroll_request) { m_applied_scroll_requests.append(applied_scroll_request); }

    // What the compositor holds on to, published whenever it changes.
    struct UserScrollGestureState {
        Vector<Web::AsyncScrollNodeStableID> boxes_in_user_scroll_gesture;
        Optional<Web::AsyncScrollNodeStableID> wheel_latched_scroller;

        bool operator==(UserScrollGestureState const&) const = default;
    };
    bool has_updates(UserScrollGestureState const&) const;
    Compositing::PendingAsyncScrollUpdates take_updates(UserScrollGestureState);

private:
    Vector<Compositing::AsyncScrollOffset> m_pending_scroll_offsets;
    u64 m_last_publication_sequence { 0 };
    Compositing::AsyncScrollOperationID m_last_allocated_operation_id { 0 };
    Vector<Compositing::AsyncScrollOperationID> m_completed_operation_ids;
    Vector<Compositing::AsyncScrollOperationID> m_operation_ids_taken_over_by_user_input;
    Vector<Compositing::StartedUserScroll> m_started_user_scrolls;
    Vector<Compositing::AppliedScrollRequest> m_applied_scroll_requests;
    Vector<Web::AsyncScrollNodeStableID> m_boxes_whose_user_scroll_gesture_ended;
    UserScrollGestureState m_published_user_scroll_gesture_state;
};

}
