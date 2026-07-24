/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Math.h>
#include <LibMedia/Audio/NullPlaybackStream.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

namespace Audio {

static constexpr u32 NULL_OUTPUT_SAMPLE_RATE = 44100;
static constexpr u32 PULL_INTERVAL_MS = 10;

class NullPlaybackStream::State final : public AtomicRefCounted<State> {
public:
    State(OutputState initial_output_state, u32 target_latency_ms, AudioDataRequestCallback data_request_callback)
        : m_data_request_callback(move(data_request_callback))
        , m_target_buffer_frames(max(1u, NULL_OUTPUT_SAMPLE_RATE * target_latency_ms / 1000))
        , m_state(initial_output_state == OutputState::Playing ? StreamState::Playing : StreamState::Suspended)
    {
    }

    ~State()
    {
        stop();
    }

    void start()
    {
        auto thread = MUST(Threading::Thread::try_create("Null Audio Output"sv, [self = NonnullRefPtr(*this)] {
            return self->thread_main();
        }));
        thread->start();
        thread->detach();
    }

    SampleSpecification sample_specification() const
    {
        return { NULL_OUTPUT_SAMPLE_RATE, ChannelMap::stereo() };
    }

    NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume()
    {
        auto promise = Core::ThreadedPromise<AK::Duration>::create();
        {
            Sync::MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            m_ready_void_promises.extend(move(m_drain_promises));
            m_state = StreamState::Playing;
            m_last_update = MonotonicTime::now();
            m_frame_debt = 0;
            m_resume_promises.append(promise);
            m_wake_condition.signal();
        }
        return promise;
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend()
    {
        auto promise = Core::ThreadedPromise<void>::create();
        {
            Sync::MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            m_state = StreamState::Draining;
            m_drain_promises.append(promise);
            m_wake_condition.signal();
        }
        return promise;
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend()
    {
        auto promise = Core::ThreadedPromise<void>::create();
        {
            Sync::MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            m_buffered_frames = 0;
            m_state = StreamState::Suspended;
            m_ready_void_promises.extend(move(m_drain_promises));
            m_ready_void_promises.append(promise);
            m_wake_condition.signal();
        }
        return promise;
    }

    void notify_data_available()
    {
        Sync::MutexLocker locker(m_mutex);
        if (m_state != StreamState::Underrun)
            return;
        m_state = StreamState::Playing;
        m_last_update = MonotonicTime::now();
        m_frame_debt = 0;
        m_wake_condition.signal();
    }

    AK::Duration total_time_played() const
    {
        return AK::Duration::from_time_units(m_frames_played.load(), 1, NULL_OUTPUT_SAMPLE_RATE);
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double)
    {
        auto promise = Core::ThreadedPromise<void>::create();
        {
            Sync::MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            m_ready_void_promises.append(promise);
            m_wake_condition.signal();
        }
        return promise;
    }

    void stop()
    {
        {
            Sync::MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped)
                return;
            m_state = StreamState::Stopped;
            m_ready_void_promises.extend(move(m_drain_promises));
            m_wake_condition.broadcast();
        }
    }

private:
    enum class StreamState {
        Playing,
        Draining,
        Suspended,
        Underrun,
        Stopped,
    };

    intptr_t thread_main()
    {
        auto const channel_count = sample_specification().channel_count();
        while (true) {
            Optional<size_t> frames_to_request;
            Vector<NonnullRefPtr<Core::ThreadedPromise<AK::Duration>>> resume_promises;
            Vector<NonnullRefPtr<Core::ThreadedPromise<void>>> ready_void_promises;
            Vector<NonnullRefPtr<Core::ThreadedPromise<void>>> drain_promises;
            bool stopped = false;
            bool inactive = false;

            {
                Sync::MutexLocker locker(m_mutex);
                resume_promises = move(m_resume_promises);
                ready_void_promises = move(m_ready_void_promises);
                if (m_state == StreamState::Stopped) {
                    ready_void_promises.extend(move(m_drain_promises));
                    stopped = true;
                }

                if (!stopped && (m_state == StreamState::Suspended || m_state == StreamState::Underrun))
                    inactive = true;

                if (!stopped && !inactive) {
                    auto now = MonotonicTime::now();
                    m_frame_debt += (now - m_last_update).to_nanoseconds() / 1e9 * NULL_OUTPUT_SAMPLE_RATE;
                    m_last_update = now;
                    // Avoid an unbounded burst of data requests after the worker was stalled for a long time.
                    m_frame_debt = min(m_frame_debt, static_cast<double>(NULL_OUTPUT_SAMPLE_RATE));
                    auto consumed_frames = min(m_buffered_frames, static_cast<size_t>(m_frame_debt));
                    m_buffered_frames -= consumed_frames;
                    m_frame_debt -= consumed_frames;
                    m_frames_played.fetch_add(consumed_frames);

                    if (m_state == StreamState::Draining && m_buffered_frames == 0) {
                        m_state = StreamState::Suspended;
                        drain_promises = move(m_drain_promises);
                    } else if (m_state == StreamState::Playing && m_buffered_frames < m_target_buffer_frames) {
                        frames_to_request = m_target_buffer_frames - m_buffered_frames;
                    }
                }
            }

            for (auto& promise : resume_promises)
                promise->resolve(total_time_played());
            for (auto& promise : ready_void_promises)
                promise->resolve();
            for (auto& promise : drain_promises)
                promise->resolve();

            if (stopped)
                return 0;

            if (inactive) {
                Sync::MutexLocker locker(m_mutex);
                if (m_state == StreamState::Suspended || m_state == StreamState::Underrun)
                    m_wake_condition.wait();
                continue;
            }

            if (frames_to_request.has_value()) {
                m_request_buffer.resize(frames_to_request.value() * channel_count);
                auto written_samples = m_data_request_callback(m_request_buffer.span());
                auto written_frames = min(frames_to_request.value(), written_samples.size() / channel_count);

                {
                    Sync::MutexLocker locker(m_mutex);
                    if (m_state == StreamState::Playing) {
                        m_buffered_frames += written_frames;
                        if (written_frames == 0 && m_buffered_frames == 0)
                            m_state = StreamState::Underrun;
                    }
                }

                continue;
            }

            {
                Sync::MutexLocker locker(m_mutex);
                if (m_state == StreamState::Playing || m_state == StreamState::Draining)
                    m_wake_condition.wait_for(AK::Duration::from_milliseconds(PULL_INTERVAL_MS));
            }
        }
    }

    AudioDataRequestCallback m_data_request_callback;
    size_t m_target_buffer_frames { 0 };

    mutable Sync::Mutex m_mutex;
    Sync::ConditionVariable m_wake_condition { m_mutex };
    StreamState m_state { StreamState::Suspended };
    Vector<NonnullRefPtr<Core::ThreadedPromise<AK::Duration>>> m_resume_promises;
    Vector<NonnullRefPtr<Core::ThreadedPromise<void>>> m_drain_promises;
    Vector<NonnullRefPtr<Core::ThreadedPromise<void>>> m_ready_void_promises;
    size_t m_buffered_frames { 0 };
    MonotonicTime m_last_update { MonotonicTime::now() };
    double m_frame_debt { 0 };
    Vector<float> m_request_buffer;
    Atomic<u64> m_frames_played { 0 };
};

NonnullRefPtr<PlaybackStream> NullPlaybackStream::create(OutputState initial_output_state, u32 target_latency_ms, AudioDataRequestCallback data_request_callback)
{
    auto state = make_ref_counted<State>(initial_output_state, target_latency_ms, move(data_request_callback));
    state->start();
    return adopt_ref(*new NullPlaybackStream(move(state)));
}

NullPlaybackStream::NullPlaybackStream(NonnullRefPtr<State> state)
    : m_state(move(state))
{
}

NullPlaybackStream::~NullPlaybackStream()
{
    m_state->stop();
}

SampleSpecification NullPlaybackStream::sample_specification() const
{
    return m_state->sample_specification();
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> NullPlaybackStream::resume()
{
    return m_state->resume();
}

NonnullRefPtr<Core::ThreadedPromise<void>> NullPlaybackStream::drain_buffer_and_suspend()
{
    return m_state->drain_buffer_and_suspend();
}

NonnullRefPtr<Core::ThreadedPromise<void>> NullPlaybackStream::discard_buffer_and_suspend()
{
    return m_state->discard_buffer_and_suspend();
}

void NullPlaybackStream::notify_data_available()
{
    m_state->notify_data_available();
}

AK::Duration NullPlaybackStream::total_time_played() const
{
    return m_state->total_time_played();
}

NonnullRefPtr<Core::ThreadedPromise<void>> NullPlaybackStream::set_volume(double volume)
{
    return m_state->set_volume(volume);
}

}
