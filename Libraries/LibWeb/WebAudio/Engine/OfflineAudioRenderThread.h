/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>
#include <LibWeb/WebAudio/Engine/OfflineAudioRenderTypes.h>

namespace Web::WebAudio::Render {

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
// https://webaudio.github.io/web-audio-api/#offline-rendering
class OfflineAudioRenderThread {
public:
    using CompletionDispatcher = Function<void()>;

    OfflineAudioRenderThread(OfflineAudioRenderRequest request, CompletionDispatcher completion_dispatcher, int suspend_write_fd);
    ~OfflineAudioRenderThread();

    void start();
    void request_abort();

    bool is_finished() const;
    Optional<OfflineAudioRenderResult> take_result();

    // Called by the control thread when OfflineAudioContext.resume() is invoked.
    // If there's a graph update, the render thread will apply it before rendering resumes.
    void request_resume(Optional<OfflineAudioGraphUpdate> updated_graph);

    // Returns the most recent analyser snapshot at the suspension boundary.
    Optional<OfflineAudioAnalyserSnapshot> take_analyser_snapshot(u32 expected_frame_index);

private:
    void rendering_thread_loop();
    void signal_completion() const;
    void signal_suspended(u32 frame_index) const;

    OfflineAudioRenderRequest m_request;
    CompletionDispatcher m_completion_dispatcher;
    int m_suspend_write_fd { -1 };

    mutable Threading::Mutex m_mutex;
    Threading::ConditionVariable m_resume_condition { m_mutex };

    bool m_abort_requested { false };
    bool m_finished { false };
    bool m_resume_requested { false };
    Optional<OfflineAudioGraphUpdate> m_pending_graph_update;
    Optional<OfflineAudioRenderResult> m_result;
    Optional<OfflineAudioAnalyserSnapshot> m_latest_analyser_snapshot;
    NonnullRefPtr<Threading::Thread> m_thread;
};

}
