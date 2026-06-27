/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Size.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/VideoEdgeQueue.h>
#include <LibMedia/VideoPresentation/PresentedFramePage.h>
#include <LibMedia/VideoPresentation/VideoPresentationClientConnection.h>

namespace Media {

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

void VideoPresentationClientConnection::create_edge(VideoSinkHandle handle, Function<void(NonnullRefPtr<DisplayingVideoSink>)> on_ready)
{
    auto edge_id = m_next_edge_id++;
    m_edge_states.set(edge_id, EdgeState { handle, move(on_ready), nullptr, nullptr, {} });
    async_create_video_edge(handle, edge_id);
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
    m_edge_states.remove(*edge_id_to_release);
    async_release_video_edge(*edge_id_to_release);
}

void VideoPresentationClientConnection::set_sink_ticking(VideoSinkHandle handle, bool ticking)
{
    for (auto& entry : m_edge_states) {
        if (entry.value.handle == handle) {
            async_set_sink_ticking(entry.key, ticking);
            return;
        }
    }
}

void VideoPresentationClientConnection::video_edge_ready(u64 edge_id, VideoEdgeQueue edge, PresentedFramePage presented_frame_page, MediaTimeReader time_reader)
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
    consumer_delegates.request_seek = [weak_connection, edge_id](AK::Duration timestamp) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_request_seek(edge_id, timestamp);
        });
    };
    consumer_delegates.release_slot = [weak_connection, edge_id](VideoFramePoolID pool_id, u32 slot_index) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_release_slot(edge_id, pool_id, slot_index);
        });
    };
    consumer_delegates.notify_space_available = [weak_connection, edge_id] {
        weak_connection.with_target([&](auto& connection) {
            connection.async_notify_space_available(edge_id);
        });
    };
    auto consumer = RemoteVideoProducer::create(move(edge), edge_state->slot_directory, move(consumer_delegates));

    auto sink = MUST(DisplayingVideoSink::try_create(time_reader));
    edge_state->time_reader = time_reader;
    sink->set_presented_frame_page(move(presented_frame_page));
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

void VideoPresentationClientConnection::announce_video_frame_slot(u64 edge_id, VideoFramePoolID pool_id, u32 slot_index, Core::AnonymousBuffer slot_buffer)
{
    if (auto edge_state = m_edge_states.get(edge_id); edge_state.has_value())
        edge_state->slot_directory->notify_slot_announced(pool_id, slot_index, move(slot_buffer));
}

void VideoPresentationClientConnection::retire_video_frame_pool(u64 edge_id, VideoFramePoolID pool_id)
{
    if (auto edge_state = m_edge_states.get(edge_id); edge_state.has_value())
        edge_state->slot_directory->notify_pool_retired(pool_id);
}

void VideoPresentationClientConnection::notify_data_available(u64 edge_id)
{
    if (auto edge_state = m_edge_states.get(edge_id); edge_state.has_value() && edge_state->consumer)
        edge_state->consumer->notify_data_available();
}

void VideoPresentationClientConnection::seek_sink(u64 edge_id, u32 requested_seek_id)
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
    edge_state->sink->seek(edge_state->time_reader->current_time());
}

}
