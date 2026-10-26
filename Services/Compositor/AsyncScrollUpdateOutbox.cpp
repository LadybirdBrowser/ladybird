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

bool AsyncScrollUpdateOutbox::has_updates(bool user_scroll_gesture_in_progress) const
{
    return !m_pending_scroll_offsets.is_empty()
        || !m_completed_operation_ids.is_empty()
        || !m_operation_ids_taken_over_by_user_input.is_empty()
        || !m_started_user_scrolls.is_empty()
        || !m_applied_scroll_requests.is_empty()
        || m_user_scroll_gesture_ended
        || user_scroll_gesture_in_progress != m_published_user_scroll_gesture_in_progress;
}

Compositing::PendingAsyncScrollUpdates AsyncScrollUpdateOutbox::take_updates(bool user_scroll_gesture_in_progress)
{
    Compositing::PendingAsyncScrollUpdates updates;
    updates.sequence = ++m_last_publication_sequence;
    AK::swap(updates.scroll_offsets, m_pending_scroll_offsets);
    AK::swap(updates.completed_operation_ids, m_completed_operation_ids);
    AK::swap(updates.operation_ids_taken_over_by_user_input, m_operation_ids_taken_over_by_user_input);
    AK::swap(updates.started_user_scrolls, m_started_user_scrolls);
    AK::swap(updates.applied_scroll_requests, m_applied_scroll_requests);
    updates.user_scroll_gesture_in_progress = user_scroll_gesture_in_progress;
    updates.user_scroll_gesture_ended = m_user_scroll_gesture_ended;
    m_user_scroll_gesture_ended = false;
    m_published_user_scroll_gesture_in_progress = updates.user_scroll_gesture_in_progress;
    return updates;
}

}
