/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibAudio/ClientConnection.h>
#include <LibAudio/RemotePlaybackStream.h>
#include <LibCore/EventLoop.h>

namespace Audio {

static size_t frames_for_latency(u32 latency_ms, u32 sample_rate)
{
    return max<size_t>(1, static_cast<size_t>(latency_ms) * sample_rate / 1000);
}

RemotePlaybackStream::RemotePlaybackStream(ClientConnection& connection, u64 stream_id, NonnullRefPtr<OutputSession> session, SampleSpecification specification, u32 ring_latency_ms, OutputState initial_state, AudioDataRequestCallback callback)
    : m_connection(connection)
    , m_session(move(session))
    , m_control_loop(connection.event_loop())
    , m_stream_id(stream_id)
    , m_sample_specification(specification)
    , m_fill_target_frames(frames_for_latency(ring_latency_ms, specification.sample_rate()))
    , m_callback(move(callback))
{
    m_connection->register_stream({}, *this);
    if (initial_state == OutputState::Playing) {
        {
            MutexLocker locker(m_mutex);
            start_playing_locked();
        }
        m_connection->send_request({}, m_stream_id, ClientConnection::StreamRequest::Resume, [](RemotePlaybackStream*) { });
    }
}

RemotePlaybackStream::~RemotePlaybackStream()
{
    m_connection->unregister_stream({}, *this);
    // The last reference may go on the pump thread. The connection is reference counted on its own thread only, so
    // the reference moves into the deferred call and is released there, along with the goodbye. The session goes after
    // it, so the server hears of the stream's end first.
    run_on_control_thread([connection = move(m_connection), session = move(m_session), stream_id = m_stream_id] {
        connection->stream_destroyed({}, stream_id);
    });
}

void RemotePlaybackStream::run_on_control_thread(Function<void()> function)
{
    // Control may come from any thread, and the IPC behind it may only be sent from the connection's loop.
    if (Core::EventLoop::is_running() && &Core::EventLoop::current() == &m_control_loop) {
        function();
        return;
    }
    m_control_loop.deferred_invoke(move(function));
}

void RemotePlaybackStream::ensure_ring_locked()
{
    if (m_ring.has_value())
        return;
    // Room for the fill target plus the slack of one pump period before the ring is full.
    auto ring = MUST(SharedAudioFrameRing::create(m_sample_specification.sample_rate(), m_sample_specification.channel_count(), m_fill_target_frames + m_fill_target_frames / 4));
    m_connection->async_attach_stream_ring(m_stream_id, ring);
    m_ring = move(ring);
}

void RemotePlaybackStream::start_playing_locked()
{
    ensure_ring_locked();
    m_state = State::Playing;
    m_waiting_for_data = false;
}

NonnullRefPtr<Core::ThreadedPromise<void>> RemotePlaybackStream::resume()
{
    auto promise = Core::ThreadedPromise<void>::create();
    run_on_control_thread([self = NonnullRefPtr(*this), promise] {
        {
            MutexLocker locker(self->m_mutex);
            if (self->m_state == State::Lost) {
                promise->resolve();
                return;
            }
            self->start_playing_locked();
        }
        self->m_connection->wake_pump();
        self->m_connection->send_request({}, self->m_stream_id, ClientConnection::StreamRequest::Resume, [promise](RemotePlaybackStream*) {
            promise->resolve();
        });
    });
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> RemotePlaybackStream::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    run_on_control_thread([self = NonnullRefPtr(*this), promise] {
        {
            MutexLocker locker(self->m_mutex);
            if (self->m_state != State::Playing && self->m_state != State::Draining) {
                promise->resolve();
                return;
            }
            self->m_state = State::Draining;
        }
        self->m_connection->send_request({}, self->m_stream_id, ClientConnection::StreamRequest::Drain, [promise](RemotePlaybackStream* stream) {
            if (stream)
                stream->drain_completed();
            promise->resolve();
        });
    });
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> RemotePlaybackStream::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    run_on_control_thread([self = NonnullRefPtr(*this), promise] {
        {
            MutexLocker locker(self->m_mutex);
            if (self->m_state == State::Lost || !self->m_ring.has_value()) {
                promise->resolve();
                return;
            }
            self->m_state = State::Suspended;
        }
        self->m_connection->send_request({}, self->m_stream_id, ClientConnection::StreamRequest::Discard, [promise](RemotePlaybackStream*) {
            promise->resolve();
        });
    });
    return promise;
}

void RemotePlaybackStream::notify_data_available()
{
    {
        MutexLocker locker(m_mutex);
        m_waiting_for_data = false;
        m_data_notification_count++;
    }
    m_connection->wake_pump();
}

NonnullRefPtr<Core::ThreadedPromise<void>> RemotePlaybackStream::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    run_on_control_thread([self = NonnullRefPtr(*this), promise, volume] {
        bool lost = false;
        {
            MutexLocker locker(self->m_mutex);
            lost = self->m_state == State::Lost;
        }
        if (!lost)
            self->m_connection->async_set_stream_volume(self->m_stream_id, static_cast<float>(volume));
        promise->resolve();
    });
    return promise;
}

void RemotePlaybackStream::fill(Vector<float>& scratch)
{
    size_t room_frames = 0;
    u32 channel_count = 0;
    u64 notifications_before_request = 0;
    auto now = MonotonicTime::now();
    auto buffer_starts_playing_at = now;
    {
        MutexLocker locker(m_mutex);
        if (m_state != State::Playing || m_waiting_for_data)
            return;
        VERIFY(m_ring.has_value());
        auto buffered_frames = m_ring->frame_capacity() - m_ring->frames_free();
        if (buffered_frames >= m_fill_target_frames)
            return;
        room_frames = m_fill_target_frames - buffered_frames;
        channel_count = m_ring->channel_count();
        notifications_before_request = m_data_notification_count;
        auto unplayed_frames = m_ring->frames_written() - m_ring->estimate_frames_played(now.nanoseconds());
        buffer_starts_playing_at = now + AK::Duration::from_time_units(static_cast<i64>(unplayed_frames), 1, m_sample_specification.sample_rate());
    }

    // The callback runs unlocked so control calls never wait on it. The ring's producer side is only touched here,
    // and the ring itself lives as long as this stream does.
    scratch.resize(room_frames * channel_count);
    auto written = m_callback(scratch.span(), buffer_starts_playing_at);
    if (written.is_empty()) {
        MutexLocker locker(m_mutex);
        // Data that was announced while the callback ran gets asked for on the next round instead of waiting on a
        // notification that already came.
        if (m_state == State::Playing && m_data_notification_count == notifications_before_request)
            m_waiting_for_data = true;
        return;
    }
    MutexLocker locker(m_mutex);
    // A discard that landed while the callback ran means these frames were never wanted.
    if (m_state != State::Playing)
        return;
    auto pushed_frames = m_ring->try_push(written);
    VERIFY(pushed_frames * channel_count == written.size());
}

void RemotePlaybackStream::drain_completed()
{
    MutexLocker locker(m_mutex);
    if (m_state == State::Draining)
        m_state = State::Suspended;
}

void RemotePlaybackStream::connection_lost()
{
    {
        MutexLocker locker(m_mutex);
        if (m_state == State::Lost)
            return;
        m_state = State::Lost;
        if (m_ring.has_value())
            m_ring->publish_consumption(MonotonicTime::now().nanoseconds(), false);
    }
    if (on_output_lost)
        on_output_lost();
}

}
