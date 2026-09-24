/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMediaClient/Client.h>
#include <LibMediaClient/RemotePlaybackManager.h>
#include <LibMediaClient/RemoteSourceBuffer.h>

namespace MediaClient {

NonnullRefPtr<RemoteSourceBuffer> RemoteSourceBuffer::create(RemotePlaybackManager& playback_manager)
{
    RefPtr<Client> client = playback_manager.client();
    u64 id = client ? client->allocate_id() : 0;
    auto source_buffer = adopt_ref(*new RemoteSourceBuffer(playback_manager, client, playback_manager.session_id(), id));
    playback_manager.register_source_buffer({}, *source_buffer);
    if (source_buffer->can_send())
        client->async_create_source_buffer(source_buffer->m_session_id, id);
    return source_buffer;
}

RemoteSourceBuffer::RemoteSourceBuffer(RemotePlaybackManager& playback_manager, RefPtr<Client> client, u64 session_id, u64 id)
    : m_playback_manager(&playback_manager)
    , m_client(move(client))
    , m_session_id(session_id)
    , m_id(id)
{
}

RemoteSourceBuffer::~RemoteSourceBuffer()
{
    if (m_playback_manager)
        m_playback_manager->unregister_source_buffer({}, *this);
    if (can_send())
        m_client->async_destroy_source_buffer(m_session_id, m_id);
}

bool RemoteSourceBuffer::can_send() const
{
    return m_client && m_client->is_open();
}

void RemoteSourceBuffer::set_content_type_subtype(StringView subtype)
{
    if (can_send())
        m_client->async_set_source_buffer_content_type(m_session_id, m_id, String::from_utf8_without_validation(subtype.bytes()));
}

void RemoteSourceBuffer::enqueue_append(ByteBuffer data)
{
    m_pending_append = move(data);
}

void RemoteSourceBuffer::abandon_append()
{
    m_pending_append.clear();
}

void RemoteSourceBuffer::send_pending_append(u64 append_generation)
{
    if (!m_pending_append.has_value())
        return;
    auto data = m_pending_append.release_value();
    if (!can_send()) {
        // The server is gone, so the append fails the way one it had started would.
        Core::deferred_invoke([self = NonnullRefPtr(*this), append_generation] {
            if (self->on_append_failed)
                self->on_append_failed(append_generation);
        });
        return;
    }
    m_append_generation_awaiting_outcome = append_generation;
    auto bytes = data.bytes();
    for (size_t piece_start = 0; piece_start < bytes.size(); piece_start += MAX_CODED_BYTES_PER_MESSAGE) {
        auto piece = bytes.slice(piece_start, min(MAX_CODED_BYTES_PER_MESSAGE, bytes.size() - piece_start));
        m_client->async_add_to_source_buffer_input(m_session_id, m_id, piece);
    }
    m_client->async_run_source_buffer_append(m_session_id, m_id, append_generation);
}

void RemoteSourceBuffer::reset_parser_state()
{
    m_published_state.append_state = Media::MediaSourceExtensions::AppendState::WaitingForSegment;
    if (can_send())
        m_client->async_reset_source_buffer_parser(m_session_id, m_id);
}

void RemoteSourceBuffer::remove_coded_frames(AK::Duration start, AK::Duration end)
{
    if (can_send())
        m_client->async_remove_source_buffer_coded_frames(m_session_id, m_id, start, end);
}

void RemoteSourceBuffer::set_mode(Media::MediaSourceExtensions::AppendMode mode)
{
    m_published_state.mode = mode;
    if (can_send())
        m_client->async_set_source_buffer_mode(m_session_id, m_id, mode);
}

void RemoteSourceBuffer::set_timestamp_offset(AK::Duration timestamp_offset)
{
    m_published_state.timestamp_offset = timestamp_offset;
    if (can_send())
        m_client->async_set_source_buffer_timestamp_offset(m_session_id, m_id, timestamp_offset);
}

void RemoteSourceBuffer::set_generate_timestamps_flag(bool flag)
{
    m_published_state.generate_timestamps_flag = flag;
    if (can_send())
        m_client->async_set_source_buffer_generate_timestamps_flag(m_session_id, m_id, flag);
}

void RemoteSourceBuffer::set_pending_initialization_segment_for_change_type_flag(bool flag)
{
    if (can_send())
        m_client->async_set_source_buffer_pending_initialization_segment_for_change_type_flag(m_session_id, m_id, flag);
}

void RemoteSourceBuffer::set_reached_end_of_stream(bool reached)
{
    if (can_send())
        m_client->async_set_source_buffer_reached_end_of_stream(m_session_id, m_id, reached);
}

void RemoteSourceBuffer::playback_manager_destroyed(Badge<RemotePlaybackManager>)
{
    m_playback_manager = nullptr;
}

void RemoteSourceBuffer::duration_received(Badge<RemotePlaybackManager>, double duration)
{
    if (on_duration_received)
        on_duration_received(duration);
}

void RemoteSourceBuffer::first_initialization_segment_received(Badge<RemotePlaybackManager>, Vector<Media::Track> audio_tracks, Vector<Media::Track> video_tracks, Vector<Media::Track> text_tracks)
{
    if (on_first_initialization_segment_received)
        on_first_initialization_segment_received(move(audio_tracks), move(video_tracks), move(text_tracks));
}

void RemoteSourceBuffer::coded_frames_processed(Badge<RemotePlaybackManager>, AK::Duration group_end_timestamp)
{
    if (on_coded_frames_processed)
        on_coded_frames_processed(group_end_timestamp);
}

void RemoteSourceBuffer::append_completed(Badge<RemotePlaybackManager>, u64 append_generation, Media::MediaSourceExtensions::PublishedState state)
{
    m_append_generation_awaiting_outcome.clear();
    m_published_state = move(state);
    if (on_append_completed)
        on_append_completed(append_generation);
}

void RemoteSourceBuffer::append_failed(Badge<RemotePlaybackManager>, u64 append_generation, Media::MediaSourceExtensions::PublishedState state)
{
    m_append_generation_awaiting_outcome.clear();
    m_published_state = move(state);
    if (on_append_failed)
        on_append_failed(append_generation);
}

void RemoteSourceBuffer::connection_lost(Badge<RemotePlaybackManager>)
{
    m_pending_append.clear();
    if (auto append_generation = m_append_generation_awaiting_outcome; append_generation.has_value()) {
        m_append_generation_awaiting_outcome.clear();
        if (on_append_failed)
            on_append_failed(*append_generation);
    }
}

void RemoteSourceBuffer::removal_completed(Badge<RemotePlaybackManager>, Media::MediaSourceExtensions::PublishedState state)
{
    m_published_state = move(state);
    if (on_removal_completed)
        on_removal_completed();
}

}
