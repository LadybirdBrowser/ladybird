/*
 * Copyright (c) 2026, the Ladybird developers.
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
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/OfflineAudioRenderTypes.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
// https://webaudio.github.io/web-audio-api/#offline-rendering
class WEB_API OfflineAudioRenderThread {
public:
    OfflineAudioRenderThread(OfflineAudioRenderRequest request, int completion_write_fd);
    ~OfflineAudioRenderThread();

    void start();

    bool is_finished() const;
    Optional<OfflineAudioRenderResult> take_result();

private:
    void rendering_thread_loop();
    void signal_completion() const;

    OfflineAudioRenderRequest m_request;
    int m_completion_write_fd { -1 };

    mutable Threading::Mutex m_mutex;

    bool m_finished { false };
    Optional<OfflineAudioRenderResult> m_result;
    NonnullRefPtr<Threading::Thread> m_thread;
};

}
