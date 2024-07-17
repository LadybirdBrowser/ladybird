/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaybackStreamPulseAudio.h"

#include <LibCore/ThreadedPromise.h>

namespace Audio {

#define TRY_OR_EXIT_THREAD(expression)                                                                       \
    ({                                                                                                       \
        auto&& __temporary_result = (expression);                                                            \
        if (__temporary_result.is_error()) [[unlikely]] {                                                    \
            warnln("Failure in PulseAudio control thread: {}", __temporary_result.error().string_literal()); \
            internal_state->exit();                                                                          \
            return 1;                                                                                        \
        }                                                                                                    \
        __temporary_result.release_value();                                                                  \
    })

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStreamPulseAudio::create(OutputState initial_state, u32 sample_rate, u8 channels, u32 target_latency_ms, AudioDataRequestCallback&& data_request_callback)
{
    VERIFY(data_request_callback);

    // Create an internal state for the control thread to hold on to.
    auto internal_state = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) InternalState()));
    auto playback_stream = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackStreamPulseAudio(internal_state)));

    // Create the control thread and start it.
    auto thread = TRY(Threading::Thread::try_create([=, data_request_callback = move(data_request_callback)]() mutable {
        auto context = TRY_OR_EXIT_THREAD(PulseAudioContext::instance());
        internal_state->set_stream(TRY_OR_EXIT_THREAD(context->create_stream(initial_state, sample_rate, channels, target_latency_ms, [data_request_callback = move(data_request_callback)](PulseAudioStream&, Bytes buffer, size_t sample_count) {
            return data_request_callback(buffer, PcmSampleFormat::Float32, sample_count);
        })));

        // PulseAudio retains the last volume it sets for an application. We want to consistently
        // start at 100% volume instead.
        TRY_OR_EXIT_THREAD(internal_state->stream()->set_volume(1.0));

        internal_state->thread_loop();
        return 0;
    },
        "Audio::PlaybackStream"sv));

    thread->start();
    thread->detach();
    return playback_stream;
}

PlaybackStreamPulseAudio::PlaybackStreamPulseAudio(NonnullRefPtr<InternalState> state)
    : m_state(move(state))
{
}

PlaybackStreamPulseAudio::~PlaybackStreamPulseAudio()
{
    m_state->exit();
}

#define TRY_OR_REJECT(expression, ...)                           \
    ({                                                           \
        auto&& __temporary_result = (expression);                \
        if (__temporary_result.is_error()) [[unlikely]] {        \
            promise->reject(__temporary_result.release_error()); \
            return __VA_ARGS__;                                  \
        }                                                        \
        __temporary_result.release_value();                      \
    })

void PlaybackStreamPulseAudio::set_underrun_callback(Function<void()> callback)
{
    m_state->enqueue([this, callback = move(callback)]() mutable {
        m_state->stream()->set_underrun_callback(move(callback));
    });
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> PlaybackStreamPulseAudio::resume()
{
    auto promise = Core::ThreadedPromise<AK::Duration>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([this, promise]() {
        TRY_OR_REJECT(m_state->stream()->resume());
        promise->resolve(TRY_OR_REJECT(m_state->stream()->total_time_played()));
    });
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamPulseAudio::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([this, promise]() {
        TRY_OR_REJECT(m_state->stream()->drain_and_suspend());
        promise->resolve();
    });
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamPulseAudio::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([this, promise]() {
        TRY_OR_REJECT(m_state->stream()->flush_and_suspend());
        promise->resolve();
    });
    return promise;
}

ErrorOr<AK::Duration> PlaybackStreamPulseAudio::total_time_played()
{
    if (m_state->stream() != nullptr)
        return m_state->stream()->total_time_played();
    return AK::Duration::zero();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamPulseAudio::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([this, promise, volume]() {
        TRY_OR_REJECT(m_state->stream()->set_volume(volume));
        promise->resolve();
    });
    return promise;
}

ErrorOr<void> PlaybackStreamPulseAudio::InternalState::check_is_running()
{
    if (m_exit)
        return Error::from_string_literal("PulseAudio control thread loop is not running");
    return {};
}

void PlaybackStreamPulseAudio::InternalState::set_stream(NonnullRefPtr<PulseAudioStream> const& stream)
{
    m_stream = stream;
}

RefPtr<PulseAudioStream> PlaybackStreamPulseAudio::InternalState::stream()
{
    return m_stream;
}

void PlaybackStreamPulseAudio::InternalState::enqueue(Function<void()>&& task)
{
    Threading::MutexLocker locker { m_mutex };
    m_tasks.enqueue(forward<Function<void()>>(task));
    m_wake_condition.signal();
}

void PlaybackStreamPulseAudio::InternalState::thread_loop()
{
    while (true) {
        auto task = [this]() -> Function<void()> {
            Threading::MutexLocker locker { m_mutex };

            while (m_tasks.is_empty() && !m_exit)
                m_wake_condition.wait();
            if (m_exit)
                return nullptr;
            return m_tasks.dequeue();
        }();
        if (!task) {
            VERIFY(m_exit);
            break;
        }
        task();
    }
}

void PlaybackStreamPulseAudio::InternalState::exit()
{
    m_exit = true;
    m_wake_condition.signal();
}

}
