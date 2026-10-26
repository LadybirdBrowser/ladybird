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
        || m_user_scroll_gesture_ended
        || user_scroll_gesture_in_progress != m_published_user_scroll_gesture_in_progress;
}

Compositing::PendingAsyncScrollUpdates AsyncScrollUpdateOutbox::take_updates(bool user_scroll_gesture_in_progress)
{
    Compositing::PendingAsyncScrollUpdates updates;
    updates.sequence = ++m_last_publication_sequence;
    AK::swap(updates.scroll_offsets, m_pending_scroll_offsets);
    for (auto const& scroll_offset : updates.scroll_offsets) {
        bool replaced = false;
        for (auto& unreconciled : m_unreconciled_scroll_offsets) {
            if (unreconciled.stable_node_id != scroll_offset.stable_node_id)
                continue;
            unreconciled.sequence = updates.sequence;
            unreconciled.compositor_scroll_offset = scroll_offset.compositor_scroll_offset;
            unreconciled.visual_viewport_scale = scroll_offset.visual_viewport_scale;
            replaced = true;
            break;
        }
        if (!replaced)
            m_unreconciled_scroll_offsets.append({ updates.sequence, scroll_offset.stable_node_id, scroll_offset.compositor_scroll_offset, scroll_offset.visual_viewport_scale });
    }
    AK::swap(updates.completed_operation_ids, m_completed_operation_ids);
    AK::swap(updates.operation_ids_taken_over_by_user_input, m_operation_ids_taken_over_by_user_input);
    AK::swap(updates.started_user_scrolls, m_started_user_scrolls);
    updates.user_scroll_gesture_in_progress = user_scroll_gesture_in_progress;
    updates.user_scroll_gesture_ended = m_user_scroll_gesture_ended;
    m_user_scroll_gesture_ended = false;
    m_published_user_scroll_gesture_in_progress = updates.user_scroll_gesture_in_progress;
    return updates;
}

Optional<AsyncScrollUpdateOutbox::VisualViewportState> AsyncScrollUpdateOutbox::unadopted_visual_viewport_state(Web::UniqueNodeID document_id) const
{
    Web::AsyncScrollNodeStableID visual_viewport { .node_id = document_id, .kind = Web::AsyncScrollNodeKind::VisualViewport };
    // The updates that are still to be published are the latest.
    for (auto const& pending : m_pending_scroll_offsets) {
        if (pending.stable_node_id == visual_viewport && pending.visual_viewport_scale.has_value())
            return VisualViewportState { pending.compositor_scroll_offset, pending.visual_viewport_scale.value() };
    }
    for (auto const& unreconciled : m_unreconciled_scroll_offsets) {
        if (unreconciled.stable_node_id == visual_viewport && unreconciled.visual_viewport_scale.has_value())
            return VisualViewportState { unreconciled.compositor_scroll_offset, unreconciled.visual_viewport_scale.value() };
    }
    return {};
}

void AsyncScrollUpdateOutbox::retire_scroll_offsets_adopted_by(u64 adopted_sequence)
{
    m_unreconciled_scroll_offsets.remove_all_matching([&](auto const& unreconciled) { return unreconciled.sequence <= adopted_sequence; });
}

}
