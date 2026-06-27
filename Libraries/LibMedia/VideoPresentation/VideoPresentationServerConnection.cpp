/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGfx/Size.h>
#include <LibMedia/VideoFramePool.h>
#include <LibMedia/VideoPresentation/VideoPresentationServerConnection.h>

namespace Media {

VideoPresentationServerConnection::VideoPresentationServerConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<VideoPresentationClientEndpoint, VideoPresentationServerEndpoint>(*this, move(transport), 1)
{
}

VideoPresentationServerConnection::~VideoPresentationServerConnection()
{
    revoke_weak_refs();
}

void VideoPresentationServerConnection::die()
{
    revoke_weak_refs();
    for (auto& entry : m_edge_states)
        PlaybackManager::release_video_edge(entry.value.handle);
    m_edge_states.clear();
}

void VideoPresentationServerConnection::create_video_edge(VideoSinkHandle video_sink_handle, u64 edge_id)
{
    if (m_edge_states.contains(edge_id)) {
        did_misbehave("create_video_edge: edge ID already exists");
        return;
    }
    for (auto const& entry : m_edge_states) {
        if (entry.value.handle == video_sink_handle) {
            did_misbehave("create_video_edge: video sink handle already has an edge");
            return;
        }
    }

    auto weak_connection = make_weak_ref();

    RemoteVideoSink::Delegates delegates;
    delegates.ring_data_available = [weak_connection, edge_id] {
        weak_connection.with_target([&](auto& connection) {
            connection.async_notify_data_available(edge_id);
        });
    };
    delegates.transmit_seek = [weak_connection, edge_id] {
        weak_connection.with_target([&](auto& connection) {
            VERIFY(connection.m_owner_thread_id.is_current_thread());
            auto edge_state = connection.m_edge_states.get(edge_id);
            if (!edge_state.has_value())
                return;
            auto requested_seek_id = ++edge_state->actual_requested_seek_id;
            connection.async_seek_sink(edge_id, requested_seek_id);
        });
    };
    delegates.transmit_time_reader = [weak_connection, edge_id](MediaTimeReader const& time_reader) {
        weak_connection.with_target([&](auto& connection) {
            VERIFY(connection.m_owner_thread_id.is_current_thread());
            if (!connection.m_edge_states.contains(edge_id))
                return;
            connection.async_update_edge_time_reader(edge_id, time_reader);
        });
    };
    delegates.announce_slot = [weak_connection, edge_id](VideoFramePoolID pool_id, u32 slot_index, Core::AnonymousBuffer slot_buffer) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_announce_video_frame_slot(edge_id, pool_id, slot_index, move(slot_buffer));
        });
    };
    delegates.retire_pool = [weak_connection, edge_id](VideoFramePoolID pool_id) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_retire_video_frame_pool(edge_id, pool_id);
        });
    };

    auto remote_edge_or_error = PlaybackManager::create_video_edge(video_sink_handle, move(delegates));
    if (remote_edge_or_error.is_error()) {
        dbgln("VideoPresentation: failed to create video edge: {}", remote_edge_or_error.error());
        return;
    }
    auto remote_edge = remote_edge_or_error.release_value();

    m_edge_states.set(edge_id, EdgeState { remote_edge.sink, video_sink_handle });
    async_video_edge_ready(edge_id, remote_edge.sink->edge(), remote_edge.sink->presented_frame_page(), remote_edge.time_reader);
    PlaybackManager::attach_video_edge(video_sink_handle, remote_edge.sink);
}

void VideoPresentationServerConnection::release_video_edge(u64 edge_id)
{
    auto it = m_edge_states.find(edge_id);
    if (it == m_edge_states.end())
        return;
    auto handle = it->value.handle;
    m_edge_states.remove(it);
    PlaybackManager::release_video_edge(handle);
}

void VideoPresentationServerConnection::request_start(u64 edge_id)
{
    if (auto it = m_edge_states.find(edge_id); it != m_edge_states.end())
        it->value.pump->start_input();
}

void VideoPresentationServerConnection::request_seek(u64 edge_id, AK::Duration timestamp)
{
    if (auto it = m_edge_states.find(edge_id); it != m_edge_states.end())
        it->value.pump->seek_upstream(timestamp);
}

void VideoPresentationServerConnection::release_slot(u64 edge_id, VideoFramePoolID pool_id, u32 slot_index)
{
    if (auto it = m_edge_states.find(edge_id); it != m_edge_states.end())
        it->value.pump->release_slot(pool_id, slot_index);
}

void VideoPresentationServerConnection::notify_space_available(u64 edge_id)
{
    if (auto it = m_edge_states.find(edge_id); it != m_edge_states.end())
        it->value.pump->notify_space_available();
}

void VideoPresentationServerConnection::notify_sink_status(u64 edge_id, PipelineStatus status, u32 requested_seek_id)
{
    auto it = m_edge_states.find(edge_id);
    if (it == m_edge_states.end())
        return;
    if (requested_seek_id != it->value.actual_requested_seek_id)
        return;
    it->value.pump->report_remote_status(status);
}

void VideoPresentationServerConnection::notify_sink_resize(u64 edge_id, u32 width, u32 height)
{
    auto it = m_edge_states.find(edge_id);
    if (it == m_edge_states.end())
        return;
    it->value.pump->report_remote_resize(Gfx::Size<u32> { width, height });
}

void VideoPresentationServerConnection::set_sink_ticking(u64 edge_id, bool ticking)
{
    auto it = m_edge_states.find(edge_id);
    if (it == m_edge_states.end())
        return;
    PlaybackManager::set_video_sink_ticking(it->value.handle, ticking);
}

}
