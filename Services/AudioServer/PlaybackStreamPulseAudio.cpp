/*
 * Copyright (c) 2023-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/PlatformAudio.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadedPromise.h>
#include <LibThreading/Thread.h>

#include <AudioServer/PlaybackStreamPulseAudio.h>

namespace Audio {

#define TRY_OR_REJECT_AND_EXIT(expression)                                                                                         \
    ({                                                                                                                             \
        auto&& __temporary_result = (expression);                                                                                  \
        if (__temporary_result.is_error()) [[unlikely]] {                                                                          \
            warnln("Failure in PulseAudio control thread: {}", __temporary_result.error().string_literal());                       \
            main_thread_event_loop.deferred_invoke([promise = move(promise), error = __temporary_result.release_error()] mutable { \
                promise->reject(move(error));                                                                                      \
            });                                                                                                                    \
            internal_state->exit();                                                                                                \
            return 1;                                                                                                              \
        }                                                                                                                          \
        __temporary_result.release_value();                                                                                        \
    })

NonnullRefPtr<PlaybackStream::CreatePromise> create_platform_playback_stream(OutputState initial_output_state, u32 target_latency_ms, PlaybackStream::AudioDataRequestCallback data_request_callback)
{
    return PlaybackStreamPulseAudio::create(initial_output_state, target_latency_ms, move(data_request_callback));
}

NonnullRefPtr<PlaybackStream::CreatePromise> PlaybackStreamPulseAudio::create(OutputState initial_state, u32 target_latency_ms, AudioDataRequestCallback&& data_request_callback)
{
    VERIFY(data_request_callback);

    auto promise = CreatePromise::construct();

    // Create an internal state for the control thread to hold on to.
    auto internal_state = MUST(adopt_nonnull_ref_or_enomem(new (nothrow) InternalState()));
    auto& main_thread_event_loop = Core::EventLoop::current();

    auto on_connection_lost = [weak_state = internal_state->make_weak_ref(), &main_thread_event_loop] {
        main_thread_event_loop.deferred_invoke([weak_state] {
            if (auto state = weak_state.strong_ref())
                state->output_lost();
        });
    };

    // Create the control thread and start it.
    auto thread = MUST(Threading::Thread::try_create("Audio Control"sv, [=, &main_thread_event_loop, data_request_callback = move(data_request_callback), on_connection_lost = move(on_connection_lost)]() mutable {
        auto context = TRY_OR_REJECT_AND_EXIT(PulseAudioContext::the());
        auto write_callback = [data_request_callback = move(data_request_callback)](PulseAudioStream&, Span<float> buffer, MonotonicTime buffer_starts_playing_at) {
            return data_request_callback(buffer, buffer_starts_playing_at);
        };
        internal_state->set_stream(TRY_OR_REJECT_AND_EXIT(context->create_stream(initial_state, target_latency_ms, move(write_callback), move(on_connection_lost))));

        // PulseAudio retains the last volume it sets for an application. We want to consistently
        // start at 100% volume instead.
        TRY_OR_REJECT_AND_EXIT(internal_state->stream()->set_volume(1.0));

        {
            // With the main loop locked, a loss of the stream can only be reported after the stream is handed over.
            auto locker = context->main_loop_locker();
            if (!internal_state->stream()->connection_is_good())
                TRY_OR_REJECT_AND_EXIT(ErrorOr<void> { Error::from_string_literal("PulseAudio stream was lost while it was being created") });
            // The stream is made on the main thread, so that it is only ever used and released there.
            main_thread_event_loop.deferred_invoke([promise = move(promise), internal_state] mutable {
                promise->resolve(adopt_ref(*new PlaybackStreamPulseAudio(internal_state)));
            });
        }

        internal_state->thread_loop();
        return 0;
    }));

    thread->start();
    thread->detach();
    return promise;
}

PlaybackStreamPulseAudio::PlaybackStreamPulseAudio(NonnullRefPtr<InternalState> state)
    : m_state(move(state))
{
}

SampleSpecification PlaybackStreamPulseAudio::sample_specification() const
{
    return m_state->stream()->sample_specification();
}

PlaybackStreamPulseAudio::~PlaybackStreamPulseAudio()
{
    m_state->set_on_output_lost(nullptr);
    m_state->exit();
}

void PlaybackStreamPulseAudio::set_on_output_lost(Function<void()> on_output_lost)
{
    m_state->set_on_output_lost(move(on_output_lost));
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

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamPulseAudio::resume()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([&state = *m_state, promise]() {
        TRY_OR_REJECT(state.stream()->resume());
        promise->resolve();
    });
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamPulseAudio::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([&state = *m_state, promise]() {
        TRY_OR_REJECT(state.stream()->drain_and_suspend());
        promise->resolve();
    });
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamPulseAudio::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([&state = *m_state, promise]() {
        TRY_OR_REJECT(state.stream()->flush_and_suspend());
        promise->resolve();
    });
    return promise;
}

void PlaybackStreamPulseAudio::notify_data_available()
{
    if (m_state->stream() != nullptr)
        m_state->stream()->notify_data_available();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamPulseAudio::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    TRY_OR_REJECT(m_state->check_is_running(), promise);
    m_state->enqueue([&state = *m_state, promise, volume]() {
        TRY_OR_REJECT(state.stream()->set_volume(volume));
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

void PlaybackStreamPulseAudio::InternalState::set_stream(NonnullRefPtr<PulseAudioStream>&& stream)
{
    m_stream = move(stream);
}

RefPtr<PulseAudioStream> const& PlaybackStreamPulseAudio::InternalState::stream()
{
    return m_stream;
}

void PlaybackStreamPulseAudio::InternalState::enqueue(Function<void()>&& task)
{
    MutexLocker locker { m_mutex };
    m_tasks.enqueue(forward<Function<void()>>(task));
    m_wake_condition.signal();
}

void PlaybackStreamPulseAudio::InternalState::thread_loop()
{
    while (true) {
        auto task = [this]() -> Function<void()> {
            MutexLocker locker { m_mutex };

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

void PlaybackStreamPulseAudio::InternalState::set_on_output_lost(Function<void()> on_output_lost)
{
    m_on_output_lost = move(on_output_lost);
}

void PlaybackStreamPulseAudio::InternalState::output_lost()
{
    if (auto on_output_lost = move(m_on_output_lost))
        on_output_lost();
}

void PlaybackStreamPulseAudio::InternalState::exit()
{
    m_exit = true;
    m_wake_condition.signal();
}

}
