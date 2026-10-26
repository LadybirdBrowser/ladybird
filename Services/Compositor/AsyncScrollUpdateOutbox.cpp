/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <Compositor/AsyncScrollUpdateOutbox.h>

namespace Compositor {

static void set_or_append_pending_scroll_offset(
    Vector<Compositing::AsyncScrollOffset>& pending_scroll_offsets,
    Compositing::AsyncScrollOffset const& scroll_offset)
{
    for (auto& existing : pending_scroll_offsets) {
        if (existing.stable_node_id == scroll_offset.stable_node_id) {
            existing.merge_later_scroll(scroll_offset);
            return;
        }
    }
    pending_scroll_offsets.append(scroll_offset);
}

void AsyncScrollUpdateOutbox::record_scroll_offsets(ReadonlySpan<Compositing::AsyncScrollOffset> scroll_offsets)
{
    for (auto const& scroll_offset : scroll_offsets)
        set_or_append_pending_scroll_offset(m_pending_scroll_offsets, scroll_offset);
}

void AsyncScrollUpdateOutbox::note_user_scroll_gesture_ended(ReadonlySpan<Web::AsyncScrollNodeStableID> stable_node_ids)
{
    for (auto stable_node_id : stable_node_ids) {
        if (!m_boxes_whose_user_scroll_gesture_ended.contains_slow(stable_node_id))
            m_boxes_whose_user_scroll_gesture_ended.append(stable_node_id);
    }
}

bool AsyncScrollUpdateOutbox::has_updates(UserScrollGestureState const& user_scroll_gesture_state) const
{
    return !m_pending_scroll_offsets.is_empty()
        || !m_completed_operation_ids.is_empty()
        || !m_operation_ids_taken_over_by_user_input.is_empty()
        || !m_started_user_scrolls.is_empty()
        || !m_applied_scroll_requests.is_empty()
        || !m_boxes_whose_user_scroll_gesture_ended.is_empty()
        || user_scroll_gesture_state != m_published_user_scroll_gesture_state;
}

Compositing::PendingAsyncScrollUpdates AsyncScrollUpdateOutbox::take_updates(UserScrollGestureState user_scroll_gesture_state)
{
    Compositing::PendingAsyncScrollUpdates updates;
    updates.sequence = ++m_last_publication_sequence;
    AK::swap(updates.scroll_offsets, m_pending_scroll_offsets);
    AK::swap(updates.completed_operation_ids, m_completed_operation_ids);
    AK::swap(updates.operation_ids_taken_over_by_user_input, m_operation_ids_taken_over_by_user_input);
    AK::swap(updates.started_user_scrolls, m_started_user_scrolls);
    AK::swap(updates.applied_scroll_requests, m_applied_scroll_requests);
    AK::swap(updates.boxes_whose_user_scroll_gesture_ended, m_boxes_whose_user_scroll_gesture_ended);
    updates.boxes_in_user_scroll_gesture = user_scroll_gesture_state.boxes_in_user_scroll_gesture;
    updates.wheel_latched_scroller = user_scroll_gesture_state.wheel_latched_scroller;
    m_published_user_scroll_gesture_state = move(user_scroll_gesture_state);
    return updates;
}

}
