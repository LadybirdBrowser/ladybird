/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <PaintServer/RenderClient/CompletionTracker.h>

namespace PaintServer {

void CompletionTracker::reset()
{
    while (!m_pending_completions.is_empty())
        m_pending_completions.dequeue();
    m_completed_release_tokens.clear();
    m_last_completed_release_token = 0;
}

void CompletionTracker::complete_release_token(ReleaseToken release_token)
{
    m_completed_release_tokens.set(release_token);
    flush_completed_tokens();
}

void CompletionTracker::enqueue_completion(SurfaceID surface_id, ReleaseToken release_token)
{
    m_pending_completions.enqueue(PendingCompletion {
        .surface_id = surface_id,
        .release_token = release_token,
    });
}

void CompletionTracker::flush_completed_tokens()
{
    while (!m_pending_completions.is_empty()) {
        auto const& pending = m_pending_completions.head();
        if (!m_completed_release_tokens.contains(pending.release_token))
            break;
        m_did_complete_callback(pending.surface_id, pending.release_token);
        m_last_completed_release_token = max(m_last_completed_release_token, pending.release_token);
        m_completed_release_tokens.remove(pending.release_token);
        m_pending_completions.dequeue();
    }
}

}
