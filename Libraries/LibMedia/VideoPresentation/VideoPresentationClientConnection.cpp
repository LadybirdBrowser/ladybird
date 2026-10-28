/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGfx/Size.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/VideoPresentation/VideoPresentationClientConnection.h>
#include <LibMediaCodecs/VideoFramePool.h>

namespace Media {

static u64 allocate_sender_id()
{
    static Atomic<u64> s_next_sender_id { 1 };
    return s_next_sender_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

VideoPresentationClientConnection::VideoPresentationClientConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<VideoPresentationClientEndpoint, VideoPresentationServerEndpoint>(*this, move(transport))
{
}

VideoPresentationClientConnection::~VideoPresentationClientConnection()
{
    revoke_weak_refs();
}

void VideoPresentationClientConnection::die()
{
    revoke_weak_refs();
    m_edge_states.clear();
}

ErrorOr<void> VideoPresentationClientConnection::create_edge(VideoSinkHandle handle, Function<void(NonnullRefPtr<DisplayingVideoSink>)> on_ready)
{
    if (m_edge_states.size() >= MAX_EDGE_COUNT)
        return Error::from_string_literal("Too many video edges");
    auto edge_id = m_next_edge_id++;
    m_edge_states.set(edge_id, EdgeState { .handle = handle, .on_ready = move(on_ready), .consumer = nullptr, .sink = nullptr, .time_reader = {}, .sender_id = allocate_sender_id(), .expected_requested_seek_id = 0 });
    async_create_video_edge(handle, edge_id);
    return {};
}

void VideoPresentationClientConnection::release_edge(VideoSinkHandle handle)
{
    Optional<u64> edge_id_to_release;
    for (auto& entry : m_edge_states) {
        if (entry.value.handle == handle) {
            edge_id_to_release = entry.key;
            break;
        }
    }
    if (!edge_id_to_release.has_value())
        return;
    if (auto consumer = m_edge_states.get(*edge_id_to_release)->consumer)
        consumer->stop_consuming();
    m_edge_states.remove(*edge_id_to_release);
    async_release_video_edge(*edge_id_to_release);
}

void VideoPresentationClientConnection::video_edge_ready(u64 edge_id, MediaTimeReader time_reader)
{
    auto edge_state = m_edge_states.get(edge_id);
    if (!edge_state.has_value())
        return;

    auto weak_connection = make_weak_ref();
    RemoteVideoProducer::Delegates consumer_delegates;
    consumer_delegates.request_start = [weak_connection, edge_id] {
        weak_connection.with_target([&](auto& connection) {
            connection.async_request_start(edge_id);
        });
    };
    consumer_delegates.request_seek = [weak_connection, edge_id](AK::Duration timestamp, InvalidateHeldData invalidate_held_data, u64 newest_received_lend_id) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_request_seek(edge_id, timestamp, invalidate_held_data, newest_received_lend_id);
        });
    };
    auto consumer = RemoteVideoProducer::create(move(consumer_delegates));

    auto sink = MUST(DisplayingVideoSink::try_create(time_reader));
    edge_state->time_reader = time_reader;
    sink->set_state_change_handler([weak_connection, edge_id](PipelineStatus status) {
        weak_connection.with_target([&](auto& connection) {
            VERIFY(connection.m_owner_thread_id.is_current_thread());
            u32 requested_seek_id = 0;
            if (auto edge_state = connection.m_edge_states.get(edge_id); edge_state.has_value())
                requested_seek_id = edge_state->expected_requested_seek_id;
            connection.async_notify_sink_status(edge_id, status, requested_seek_id);
        });
    });
    sink->set_resize_handler([weak_connection, edge_id](Gfx::Size<u32> size) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_notify_sink_resize(edge_id, size.width(), size.height());
        });
    });
    MUST(sink->connect_input(consumer));

    edge_state->consumer = consumer;
    edge_state->sink = sink;
    auto on_ready = move(edge_state->on_ready);
    if (on_ready)
        on_ready(sink);
}

void VideoPresentationClientConnection::update_edge_time_reader(u64 edge_id, MediaTimeReader time_reader)
{
    auto edge_state = m_edge_states.get(edge_id);
    if (!edge_state.has_value())
        return;
    edge_state->time_reader = time_reader;
    if (edge_state->sink)
        edge_state->sink->set_time_reader(move(time_reader));
}

void VideoPresentationClientConnection::lend_frame(u64 edge_id, u64 lend_id, u32 seek_id, VideoFrameMetadata metadata, VideoFramePixels pixels)
{
    // The frame may be dropped on any thread.
    auto release = [weak_connection = make_weak_ref(), edge_id, lend_id] {
        weak_connection.with_target([&](auto& connection) {
            connection.async_release_frame(edge_id, lend_id);
        });
    };

    auto edge_state = m_edge_states.get(edge_id);
    if (!edge_state.has_value() || !edge_state->consumer) {
        release();
        return;
    }
    auto frame_or_error = create_received_video_frame(metadata, move(pixels), edge_state->sender_id, move(release));
    if (frame_or_error.is_error()) {
        dbgln("VideoPresentation: Dropping a lent frame that does not fit its storage: {}", frame_or_error.error());
        return;
    }
    auto frame = frame_or_error.release_value();
    // Every frame comes with a fresh mapping of its buffer, which is read in full to upload it.
    frame->pixel_buffer().populate_for_reading();
    edge_state->consumer->receive_frame(lend_id, seek_id, move(frame));
}

void VideoPresentationClientConnection::edge_status_changed(u64 edge_id, PipelineStatus status, u32 seek_id)
{
    if (auto edge_state = m_edge_states.get(edge_id); edge_state.has_value() && edge_state->consumer)
        edge_state->consumer->receive_status(status, seek_id);
}

void VideoPresentationClientConnection::notify_clock_resumed(u64 edge_id)
{
    if (auto edge_state = m_edge_states.get(edge_id); edge_state.has_value() && edge_state->consumer)
        edge_state->consumer->notify_clock_resumed();
}

void VideoPresentationClientConnection::seek_sink(u64 edge_id, u32 requested_seek_id, InvalidateHeldData invalidate_held_data)
{
    auto edge_state = m_edge_states.get(edge_id);
    if (!edge_state.has_value())
        return;
    edge_state->expected_requested_seek_id = requested_seek_id;
    if (!edge_state->sink || !edge_state->time_reader.has_value())
        return;
    // The edge carries no timestamp so that seeking forward repeatedly doesn't cause the timestamps between the seeks
    // and the time reader to become discontinuous. Otherwise, DisplayingVideoSink::update() may read the time for a
    // seek that has not been processed yet, causing the sink to discard a frame that it needs to consult for the
    // incoming seek. With this fix, the spurious seek to the timestamp that was already read will simply hit the cache
    // anyway, so there is no harm here.
    edge_state->sink->seek(edge_state->time_reader->current_time(), invalidate_held_data);
}

}
