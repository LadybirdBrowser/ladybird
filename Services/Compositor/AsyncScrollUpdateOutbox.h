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
    // Offsets handed to WebContent that no snapshot of its has incorporated yet; the compositor keeps
    // reapplying them over the main-thread state that arrives in the meantime.
    struct UnreconciledScrollOffset {
        u64 sequence { 0 };
        Web::AsyncScrollNodeStableID stable_node_id;
        Gfx::FloatPoint compositor_scroll_offset;
        Optional<float> visual_viewport_scale;
    };

    struct VisualViewportState {
        Gfx::FloatPoint scroll_offset;
        float scale { 1 };
    };

    Compositing::AsyncScrollOperationID allocate_operation_id() { return ++m_last_allocated_operation_id; }
    void complete_operation(Compositing::AsyncScrollOperationID operation_id) { m_completed_operation_ids.append(operation_id); }
    void note_operation_taken_over_by_user_input(Compositing::AsyncScrollOperationID operation_id) { m_operation_ids_taken_over_by_user_input.append(operation_id); }
    void record_scroll_offsets(ReadonlySpan<Compositing::AsyncScrollOffset>);
    void record_started_user_scroll(Compositing::StartedUserScroll started_user_scroll) { m_started_user_scrolls.append(move(started_user_scroll)); }
    void note_user_scroll_gesture_ended() { m_user_scroll_gesture_ended = true; }

    bool has_updates(bool user_scroll_gesture_in_progress) const;
    Compositing::PendingAsyncScrollUpdates take_updates(bool user_scroll_gesture_in_progress);

    ReadonlySpan<UnreconciledScrollOffset> unreconciled_scroll_offsets() const { return m_unreconciled_scroll_offsets; }
    // Where the compositor zoomed and panned the document's visual viewport to, until WebContent has adopted it.
    Optional<VisualViewportState> unadopted_visual_viewport_state(Web::UniqueNodeID document_id) const;
    void retire_scroll_offsets_adopted_by(u64 adopted_sequence);

private:
    Vector<Compositing::AsyncScrollOffset> m_pending_scroll_offsets;
    Vector<UnreconciledScrollOffset> m_unreconciled_scroll_offsets;
    u64 m_last_publication_sequence { 0 };
    Compositing::AsyncScrollOperationID m_last_allocated_operation_id { 0 };
    Vector<Compositing::AsyncScrollOperationID> m_completed_operation_ids;
    Vector<Compositing::AsyncScrollOperationID> m_operation_ids_taken_over_by_user_input;
    Vector<Compositing::StartedUserScroll> m_started_user_scrolls;
    bool m_user_scroll_gesture_ended { false };
    bool m_published_user_scroll_gesture_in_progress { false };
};

}
