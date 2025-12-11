/*
 * Copyright (c) 2023-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PlaybackStream.h"
#include "PulseAudioWrappers.h"
#include <AK/Queue.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace Audio {

class PlaybackStreamPulseAudio final
    : public PlaybackStream {
public:
    static ErrorOr<NonnullRefPtr<PlaybackStream>> create(OutputState, u32 target_latency_ms, SampleSpecificationCallback&&, AudioDataRequestCallback&&);

    virtual void set_underrun_callback(Function<void()>) override;

    virtual NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;

    virtual AK::Duration total_time_played() const override;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;

private:
    // This struct is kept alive until the control thread exits to prevent a use-after-free without blocking on
    // the UI thread.
    class InternalState : public AtomicRefCounted<InternalState> {
    public:
        void set_stream(NonnullRefPtr<PulseAudioStream>&&);
        RefPtr<PulseAudioStream> stream();

        void enqueue(Function<void()>&&);
        void thread_loop();
        ErrorOr<void> check_is_running();
        void exit();

    private:
        RefPtr<PulseAudioStream> m_stream { nullptr };

        Queue<Function<void()>> m_tasks;
        Threading::Mutex m_mutex;
        Threading::ConditionVariable m_wake_condition { m_mutex };

        Atomic<bool> m_exit { false };
    };

    PlaybackStreamPulseAudio(NonnullRefPtr<InternalState>);
    ~PlaybackStreamPulseAudio();

    RefPtr<InternalState> m_state;
};

}
