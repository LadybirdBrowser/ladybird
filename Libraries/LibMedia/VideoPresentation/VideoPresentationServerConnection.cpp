/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGfx/Size.h>
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
        PlaybackManager::release_video_edge({}, entry.value.handle, *entry.value.pump);
    m_edge_states.clear();
    m_released_edge_pumps_with_lent_frames.clear();
}

void VideoPresentationServerConnection::create_video_edge(VideoSinkHandle video_sink_handle, u64 edge_id)
{
    if (m_edge_states.contains(edge_id) || m_pending_edge_handles_by_edge_id.contains(edge_id)) {
        did_misbehave("create_video_edge: edge ID already exists");
        return;
    }
    for (auto const& entry : m_edge_states) {
        if (entry.value.handle == video_sink_handle) {
            did_misbehave("create_video_edge: video sink handle already has an edge");
            return;
        }
    }
    for (auto const& entry : m_pending_edge_handles_by_edge_id) {
        if (entry.value == video_sink_handle) {
            did_misbehave("create_video_edge: video sink handle already has a pending edge");
            return;
        }
    }

    if (!PlaybackManager::has_video_sink_handle({}, video_sink_handle)) {
        m_pending_edge_handles_by_edge_id.set(edge_id, video_sink_handle);
        return;
    }
    create_registered_video_edge(video_sink_handle, edge_id);
}

void VideoPresentationServerConnection::retry_pending_video_edges()
{
    auto pending_edges = move(m_pending_edge_handles_by_edge_id);
    for (auto const& [edge_id, video_sink_handle] : pending_edges) {
        if (PlaybackManager::has_video_sink_handle({}, video_sink_handle))
            create_registered_video_edge(video_sink_handle, edge_id);
        else
            m_pending_edge_handles_by_edge_id.set(edge_id, video_sink_handle);
    }
}

void VideoPresentationServerConnection::create_registered_video_edge(VideoSinkHandle video_sink_handle, u64 edge_id)
{
    auto weak_connection = make_weak_ref();

    RemoteVideoSink::Delegates delegates;
    delegates.send_frame = [weak_connection, edge_id](RemoteVideoSink::LentFrame lent_frame) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_lend_frame(edge_id, lent_frame.lend_id, lent_frame.seek_id, lent_frame.metadata, move(lent_frame.pixels));
        });
    };
    delegates.send_status = [weak_connection, edge_id](PipelineStatus status, u32 seek_id) {
        weak_connection.with_target([&](auto& connection) {
            connection.async_edge_status_changed(edge_id, status, seek_id);
        });
    };
    delegates.notify_clock_resumed = [weak_connection, edge_id] {
        weak_connection.with_target([&](auto& connection) {
            connection.async_notify_clock_resumed(edge_id);
        });
    };
    delegates.transmit_seek = [weak_connection, edge_id](InvalidateHeldData invalidate_held_data) {
        weak_connection.with_target([&](auto& connection) {
            VERIFY(connection.m_owner_thread_id.is_current_thread());
            auto edge_state = connection.m_edge_states.get(edge_id);
            if (!edge_state.has_value())
                return;
            auto requested_seek_id = ++edge_state->actual_requested_seek_id;
            connection.async_seek_sink(edge_id, requested_seek_id, invalidate_held_data);
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

    auto remote_edge_or_error = PlaybackManager::create_video_edge({}, video_sink_handle, move(delegates));
    if (remote_edge_or_error.is_error()) {
        dbgln("VideoPresentation: failed to create video edge: {}", remote_edge_or_error.error());
        return;
    }
    auto remote_edge = remote_edge_or_error.release_value();

    m_edge_states.set(edge_id, EdgeState { remote_edge.sink, video_sink_handle });
    async_video_edge_ready(edge_id, remote_edge.time_reader);
    PlaybackManager::attach_video_edge({}, video_sink_handle, remote_edge.sink);
}

void VideoPresentationServerConnection::release_video_edge(u64 edge_id)
{
    m_pending_edge_handles_by_edge_id.remove(edge_id);
    auto it = m_edge_states.find(edge_id);
    if (it == m_edge_states.end())
        return;
    auto handle = it->value.handle;
    auto pump = it->value.pump;
    m_edge_states.remove(it);
    PlaybackManager::release_video_edge({}, handle, *pump);

    pump->stop();
    if (pump->has_lent_frames())
        m_released_edge_pumps_with_lent_frames.set(edge_id, move(pump));
}

void VideoPresentationServerConnection::request_start(u64 edge_id)
{
    if (auto it = m_edge_states.find(edge_id); it != m_edge_states.end())
        it->value.pump->start_input();
}

void VideoPresentationServerConnection::request_seek(u64 edge_id, AK::Duration timestamp, InvalidateHeldData invalidate_held_data, u64 newest_received_lend_id)
{
    if (auto it = m_edge_states.find(edge_id); it != m_edge_states.end())
        it->value.pump->seek_upstream(timestamp, invalidate_held_data, newest_received_lend_id);
}

void VideoPresentationServerConnection::release_frame(u64 edge_id, u64 lend_id)
{
    if (auto it = m_edge_states.find(edge_id); it != m_edge_states.end()) {
        it->value.pump->release_frame(lend_id);
        return;
    }
    auto released_pump = m_released_edge_pumps_with_lent_frames.get(edge_id);
    if (!released_pump.has_value())
        return;
    released_pump.value()->release_frame(lend_id);
    if (!released_pump.value()->has_lent_frames())
        m_released_edge_pumps_with_lent_frames.remove(edge_id);
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

}
