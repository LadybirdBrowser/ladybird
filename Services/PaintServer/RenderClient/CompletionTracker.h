/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/Queue.h>
#include <PaintServer/RenderClient/Types.h>

namespace PaintServer {

class CompletionTracker {
public:
    explicit CompletionTracker(Function<void(SurfaceID, ReleaseToken)> did_complete_callback)
        : m_did_complete_callback(move(did_complete_callback))
    {
    }

    void reset();

    void complete_release_token(ReleaseToken release_token);
    void enqueue_completion(SurfaceID surface_id, ReleaseToken release_token);

    ReleaseToken last_completed_release_token() const { return m_last_completed_release_token; }

private:
    void flush_completed_tokens();

    Function<void(SurfaceID, ReleaseToken)> m_did_complete_callback;
    ReleaseToken m_last_completed_release_token { 0 };
    Queue<PendingCompletion> m_pending_completions;
    HashTable<ReleaseToken> m_completed_release_tokens;
};

}
