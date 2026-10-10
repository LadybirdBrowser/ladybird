/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/Math.h>
#include <AK/Mutex.h>
#include <LibMedia/Audio/NullPlaybackStream.h>
#include <LibThreading/Thread.h>

namespace Audio {

static constexpr u32 NULL_OUTPUT_SAMPLE_RATE = 44100;
static constexpr u32 PERIOD_MS = 10;
static constexpr i64 PERIOD_FRAMES = NULL_OUTPUT_SAMPLE_RATE * PERIOD_MS / 1000;

// The played position is derived from the wall clock on demand, advancing from an anchor while the stream is playing
// or draining, and never passing the frames that have been written. Data is requested one period at a time to keep a
// target amount written ahead of the played position, like a device buffer.
class NullPlaybackStream::State final : public AtomicRefCounted<State> {
public:
    State(OutputState initial_output_state, u32 target_latency_ms, AudioDataRequestCallback data_request_callback)
        : m_data_request_callback(move(data_request_callback))
        , m_target_lookahead_frames(max(PERIOD_FRAMES, static_cast<i64>(NULL_OUTPUT_SAMPLE_RATE) * target_latency_ms / 1000))
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
            MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            complete_pending_drains_while_locked();
            set_state_while_locked(StreamState::Playing);
            m_awaiting_data = false;
            auto resume_time = AK::Duration::from_time_units(m_anchor_frames_played, 1, NULL_OUTPUT_SAMPLE_RATE);
            m_ready_completions.append([promise, resume_time]() mutable { promise->resolve(move(resume_time)); });
            signal_while_locked();
        }
        return promise;
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend()
    {
        auto promise = Core::ThreadedPromise<void>::create();
        {
            MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            set_state_while_locked(StreamState::Draining);
            m_drain_promises.append(promise);
            signal_while_locked();
        }
        return promise;
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend()
    {
        auto promise = Core::ThreadedPromise<void>::create();
        {
            MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            set_state_while_locked(StreamState::Suspended);
            m_frames_written = m_anchor_frames_played;
            complete_pending_drains_while_locked();
            m_ready_completions.append([promise] { promise->resolve(); });
            signal_while_locked();
        }
        return promise;
    }

    void notify_data_available()
    {
        MutexLocker locker(m_mutex);
        m_awaiting_data = false;
        m_data_notified = true;
        if (m_state == StreamState::Underrun)
            set_state_while_locked(StreamState::Playing);
        signal_while_locked();
    }

    AK::Duration total_time_played() const
    {
        MutexLocker locker(m_mutex);
        return AK::Duration::from_time_units(frames_played_while_locked(MonotonicTime::now()), 1, NULL_OUTPUT_SAMPLE_RATE);
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double)
    {
        auto promise = Core::ThreadedPromise<void>::create();
        {
            MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped) {
                promise->reject(Error::from_string_literal("Null playback stream has stopped"));
                return promise;
            }
            m_ready_completions.append([promise] { promise->resolve(); });
            signal_while_locked();
        }
        return promise;
    }

    void stop()
    {
        {
            MutexLocker locker(m_mutex);
            if (m_state == StreamState::Stopped)
                return;
            set_state_while_locked(StreamState::Stopped);
            complete_pending_drains_while_locked();
            m_signal_count++;
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

    void complete_pending_drains_while_locked()
    {
        for (auto& promise : m_drain_promises)
            m_ready_completions.append([promise] { promise->resolve(); });
        m_drain_promises.clear();
    }

    void signal_while_locked()
    {
        m_signal_count++;
        m_wake_condition.signal();
    }

    bool is_advancing_while_locked() const
    {
        return m_state == StreamState::Playing || m_state == StreamState::Draining;
    }

    i64 elapsed_frames_since_anchor_while_locked(MonotonicTime now) const
    {
        auto elapsed_nanoseconds = max<i64>(0, (now - m_anchor_time).to_nanoseconds());
        return elapsed_nanoseconds * NULL_OUTPUT_SAMPLE_RATE / 1'000'000'000;
    }

    i64 frames_played_while_locked(MonotonicTime now) const
    {
        if (!is_advancing_while_locked())
            return m_anchor_frames_played;
        return min(m_frames_written, m_anchor_frames_played + elapsed_frames_since_anchor_while_locked(now));
    }

    void reanchor_while_locked(MonotonicTime now)
    {
        if (!is_advancing_while_locked()) {
            m_anchor_time = now;
            return;
        }
        auto elapsed_frames = elapsed_frames_since_anchor_while_locked(now);
        if (m_anchor_frames_played + elapsed_frames < m_frames_written) {
            // Advance by whole frames, keeping the remainder in the anchor time so that reanchoring doesn't drift.
            m_anchor_time = m_anchor_time + AK::Duration::from_time_units(elapsed_frames, 1, NULL_OUTPUT_SAMPLE_RATE);
            m_anchor_frames_played += elapsed_frames;
            return;
        }
        // Everything written has been played, so the time since then was spent in underrun.
        m_anchor_frames_played = m_frames_written;
        m_anchor_time = now;
    }

    void set_state_while_locked(StreamState state)
    {
        reanchor_while_locked(MonotonicTime::now());
        m_state = state;
    }

    intptr_t thread_main()
    {
        auto const channel_count = sample_specification().channel_count();
        while (true) {
            Optional<i64> frames_to_request;
            Optional<i64> wake_at_played_frame;
            Vector<Function<void()>> ready_completions;
            bool stopped = false;
            u64 signal_count_at_decision;

            {
                MutexLocker locker(m_mutex);
                signal_count_at_decision = m_signal_count;

                auto now = MonotonicTime::now();
                auto frames_played = frames_played_while_locked(now);
                switch (m_state) {
                case StreamState::Stopped:
                    complete_pending_drains_while_locked();
                    stopped = true;
                    break;
                case StreamState::Playing:
                    if (m_awaiting_data) {
                        // Like a device underrun, stop requesting data until more is available, but keep playing
                        // out what was already written.
                        if (frames_played >= m_frames_written)
                            set_state_while_locked(StreamState::Underrun);
                        else
                            wake_at_played_frame = m_frames_written;
                    } else if (m_frames_written - frames_played <= m_target_lookahead_frames - PERIOD_FRAMES) {
                        frames_to_request = m_target_lookahead_frames - (m_frames_written - frames_played);
                        m_data_notified = false;
                    } else {
                        wake_at_played_frame = m_frames_written - m_target_lookahead_frames + PERIOD_FRAMES;
                    }
                    break;
                case StreamState::Draining:
                    if (frames_played >= m_frames_written) {
                        set_state_while_locked(StreamState::Suspended);
                        complete_pending_drains_while_locked();
                    } else {
                        wake_at_played_frame = m_frames_written;
                    }
                    break;
                case StreamState::Suspended:
                case StreamState::Underrun:
                    break;
                }
                ready_completions = move(m_ready_completions);
            }

            for (auto& completion : ready_completions)
                completion();

            if (stopped)
                return 0;

            if (frames_to_request.has_value()) {
                auto requested_frames = static_cast<size_t>(frames_to_request.value());
                m_request_buffer.resize(requested_frames * channel_count);
                auto written_samples = m_data_request_callback(m_request_buffer.span());
                auto written_frames = min(requested_frames, written_samples.size() / channel_count);

                MutexLocker locker(m_mutex);
                if (is_advancing_while_locked()) {
                    reanchor_while_locked(MonotonicTime::now());
                    m_frames_written += static_cast<i64>(written_frames);
                    // A notification that arrived during the request means that more data may already be available.
                    if (m_state == StreamState::Playing && written_frames < requested_frames && !m_data_notified)
                        m_awaiting_data = true;
                }
                continue;
            }

            MutexLocker locker(m_mutex);
            // Anything signaled since the decision above may have changed what to do next.
            if (m_signal_count != signal_count_at_decision)
                continue;
            if (wake_at_played_frame.has_value()) {
                // If this wakes before the frame is reached, the next iteration waits for the remainder.
                auto frames_until_wake = *wake_at_played_frame - frames_played_while_locked(MonotonicTime::now());
                if (frames_until_wake > 0)
                    m_wake_condition.wait_for(AK::Duration::from_time_units(frames_until_wake, 1, NULL_OUTPUT_SAMPLE_RATE));
            } else if (m_state == StreamState::Suspended || m_state == StreamState::Underrun) {
                m_wake_condition.wait();
            }
        }
    }

    AudioDataRequestCallback m_data_request_callback;
    i64 m_target_lookahead_frames { 0 };

    mutable Mutex m_mutex;
    ConditionVariable m_wake_condition { m_mutex };
    StreamState m_state { StreamState::Suspended };
    Vector<NonnullRefPtr<Core::ThreadedPromise<void>>> m_drain_promises;
    // NB: Keep all ready controls in one queue so a resume cannot overtake the drain it completes.
    Vector<Function<void()>> m_ready_completions;

    i64 m_frames_written { 0 };
    i64 m_anchor_frames_played { 0 };
    MonotonicTime m_anchor_time { MonotonicTime::now() };
    bool m_awaiting_data { false };
    bool m_data_notified { false };
    u64 m_signal_count { 0 };

    Vector<float> m_request_buffer;
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
