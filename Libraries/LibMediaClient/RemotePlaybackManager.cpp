/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/VideoFrame.h>
#include <LibMediaClient/Client.h>
#include <LibMediaClient/RemoteMediaStream.h>
#include <LibMediaClient/RemotePlaybackManager.h>
#include <LibMediaClient/RemoteSourceBuffer.h>

namespace MediaClient {

NonnullOwnPtr<RemotePlaybackManager> RemotePlaybackManager::create(bool audio_output_disabled)
{
    auto client_or_error = Client::acquire();
    if (client_or_error.is_error()) {
        dbgln("MediaClient: Could not reach the media server for playback: {}", client_or_error.error());
        auto playback_manager = adopt_own(*new RemotePlaybackManager(nullptr, 0));
        // The owner installs its error handler after creation, so the failure reaches it from the event loop.
        Core::deferred_invoke([playback_manager = playback_manager->make_weak_ptr()] {
            if (playback_manager)
                playback_manager->handle_connection_lost();
        });
        return playback_manager;
    }
    auto client = client_or_error.release_value();
    auto playback_manager = adopt_own(*new RemotePlaybackManager(client, client->allocate_id()));
    client->register_playback_manager({}, *playback_manager);
    client->async_create_playback_session(playback_manager->session_id(), audio_output_disabled);
    return playback_manager;
}

RemotePlaybackManager::RemotePlaybackManager(RefPtr<Client> client, u64 session_id)
    : m_client(move(client))
    , m_session_id(session_id)
{
}

RemotePlaybackManager::~RemotePlaybackManager()
{
    for (auto& [id, source_buffer] : m_source_buffers)
        source_buffer->playback_manager_destroyed({});
    if (!m_client)
        return;
    m_client->unregister_playback_manager({}, *this);
    if (m_client->is_open())
        m_client->async_destroy_playback_session(m_session_id);
}

bool RemotePlaybackManager::can_send() const
{
    return m_client && m_client->is_open();
}

void RemotePlaybackManager::set_duration(AK::Duration duration)
{
    m_duration_was_provided = true;
    m_duration = duration;
    if (can_send())
        m_client->async_set_duration(m_session_id, duration);
}

AK::Duration RemotePlaybackManager::current_time() const
{
    AK::Duration time;
    switch (m_state) {
    case Media::PlaybackState::Ended:
        return m_duration;
    case Media::PlaybackState::Seeking:
        time = m_seek_timestamp;
        break;
    default:
        if (m_time_reader.has_value())
            time = m_time_reader->current_time();
        else
            time = m_seek_timestamp;
        break;
    }
    return min(time, m_duration);
}

Media::VideoSinkHandle RemotePlaybackManager::reserve_video_sink_handle(Media::Track const& track)
{
    auto handle = Media::allocate_video_sink_handle();
    m_video_sinks.set(handle, VideoSink {});
    if (can_send())
        m_client->async_reserve_video_sink(m_session_id, track, handle);
    return handle;
}

void RemotePlaybackManager::disable_video_sink_by_handle(Media::VideoSinkHandle handle)
{
    m_video_sinks.remove(handle);
    if (can_send())
        m_client->async_disable_video_sink(m_session_id, handle);
}

void RemotePlaybackManager::set_video_sink_ticking(Media::VideoSinkHandle handle, bool ticking)
{
    if (can_send())
        m_client->async_set_video_sink_ticking(m_session_id, handle, ticking);
}

void RemotePlaybackManager::detach_video_sink(Media::VideoSinkHandle handle)
{
    if (auto sink = m_video_sinks.get(handle); sink.has_value())
        sink->presented_frame_page.clear();
    if (can_send())
        m_client->async_detach_video_sink(m_session_id, handle);
}

void RemotePlaybackManager::set_video_resize_handler(Media::VideoSinkHandle handle, Function<void(Gfx::Size<u32>)> handler)
{
    if (auto sink = m_video_sinks.get(handle); sink.has_value())
        sink->on_resize = move(handler);
}

RefPtr<Media::VideoFrame> RemotePlaybackManager::current_presented_frame(Media::VideoSinkHandle handle)
{
    auto sink = m_video_sinks.get(handle);
    if (!sink.has_value() || !sink->presented_frame_page.has_value())
        return nullptr;
    auto frame_handle = sink->presented_frame_page->read();
    if (!frame_handle.has_value())
        return nullptr;

    // The slot's storage is mapped on first sight and kept until the server retires the pool. No hold is taken:
    // the resolved frame validates the slot's acquisition ID around its reads, so a recycled slot fails the read.
    auto frame = sink->slot_directory->resolve_frame(*frame_handle, [] { });
    if (frame)
        return frame;
    if (!can_send())
        return nullptr;

    auto response = m_client->send_sync_but_allow_failure<Messages::MediaServer::MapPresentedFrameSlot>(m_session_id, handle, frame_handle->pool_id, frame_handle->slot_index);
    if (!response || !response->slot_buffer().has_value())
        return nullptr;
    sink->slot_directory->notify_slot_announced(frame_handle->pool_id, frame_handle->slot_index, response->take_slot_buffer().release_value(), response->take_surface());
    return sink->slot_directory->resolve_frame(*frame_handle, [] { });
}

void RemotePlaybackManager::enable_an_audio_track(Media::Track const& track)
{
    if (can_send())
        m_client->async_set_audio_track_enabled(m_session_id, track, true);
}

void RemotePlaybackManager::disable_an_audio_track(Media::Track const& track)
{
    if (can_send())
        m_client->async_set_audio_track_enabled(m_session_id, track, false);
}

void RemotePlaybackManager::add_media_source(RemoteMediaStream& stream)
{
    if (can_send())
        m_client->async_add_media_stream_source(m_session_id, stream.id());
}

void RemotePlaybackManager::start()
{
    if (can_send())
        m_client->async_start_playback(m_session_id);
}

void RemotePlaybackManager::play()
{
    if (can_send())
        m_client->async_play(m_session_id);
}

void RemotePlaybackManager::pause()
{
    if (can_send())
        m_client->async_pause(m_session_id);
}

void RemotePlaybackManager::seek(AK::Duration timestamp, Media::SeekMode mode)
{
    m_seek_timestamp = timestamp;
    m_latest_seek_request_id++;
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "RemotePlaybackManager({}): Requesting seek {} to {} ({}) in {}", m_session_id, m_latest_seek_request_id, timestamp, mode, m_state);
    if (can_send())
        m_client->async_seek(m_session_id, m_latest_seek_request_id, timestamp, mode);
    if (m_state == Media::PlaybackState::Seeking)
        return;
    m_state = Media::PlaybackState::Seeking;
    m_available_data = Media::AvailableData::Current;
    dispatch_state_change();
}

void RemotePlaybackManager::set_volume(double volume)
{
    if (can_send())
        m_client->async_set_volume(m_session_id, volume);
}

void RemotePlaybackManager::set_playback_rate(float rate)
{
    if (can_send())
        m_client->async_set_playback_rate(m_session_id, rate);
}

void RemotePlaybackManager::dispatch_state_change() const
{
    if (on_playback_state_change)
        on_playback_state_change();
}

void RemotePlaybackManager::register_source_buffer(Badge<RemoteSourceBuffer>, RemoteSourceBuffer& source_buffer)
{
    m_source_buffers.set(source_buffer.id(), &source_buffer);
}

void RemotePlaybackManager::unregister_source_buffer(Badge<RemoteSourceBuffer>, RemoteSourceBuffer& source_buffer)
{
    m_source_buffers.remove(source_buffer.id());
}

RemoteSourceBuffer* RemotePlaybackManager::find_source_buffer(u64 source_buffer_id)
{
    return m_source_buffers.get(source_buffer_id).value_or(nullptr);
}

void RemotePlaybackManager::handle_connection_lost()
{
    m_client = nullptr;
    for (auto& [id, source_buffer] : m_source_buffers)
        source_buffer->connection_lost({});
    dispatch_error(Media::DecoderError::with_description(Media::DecoderErrorCategory::Unknown, "The media server is gone"sv));
}

void RemotePlaybackManager::dispatch_error(Media::DecoderError&& error)
{
    if (m_is_in_error_state)
        return;
    m_is_in_error_state = true;
    if (on_error)
        on_error(move(error));
}

void RemotePlaybackManager::clock_changed(Badge<Client>, Media::MediaTimeReader time_reader)
{
    m_time_reader = move(time_reader);
}

void RemotePlaybackManager::metadata_parsed(Badge<Client>, Vector<Media::Track> const& audio_tracks, Vector<Media::Track> const& video_tracks, Optional<Media::Track> preferred_audio_track, Optional<Media::Track> preferred_video_track, Optional<AK::UnixDateTime> start_time_realtime)
{
    m_audio_tracks.extend(audio_tracks);
    m_video_tracks.extend(video_tracks);
    if (!m_preferred_audio_track.has_value())
        m_preferred_audio_track = preferred_audio_track;
    if (!m_preferred_video_track.has_value())
        m_preferred_video_track = preferred_video_track;
    m_start_time_realtime = start_time_realtime;

    if (on_track_added) {
        for (auto const& track : audio_tracks)
            on_track_added(track);
        for (auto const& track : video_tracks)
            on_track_added(track);
    }
    if (on_metadata_parsed)
        on_metadata_parsed();
}

void RemotePlaybackManager::duration_changed(Badge<Client>, AK::Duration duration)
{
    // A demuxed duration reported before the server saw the provided one must not replace it.
    if (m_duration_was_provided)
        return;
    m_duration = duration;
    if (on_duration_change)
        on_duration_change(duration);
}

void RemotePlaybackManager::state_changed(Badge<Client>, u64 applied_seek_request_id, Media::PlaybackState state, bool is_playing, Media::AvailableData available_data, AK::Duration current_time)
{
    if (applied_seek_request_id != m_latest_seek_request_id) {
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "RemotePlaybackManager({}): Dropping {} reported after seek {}, latest is {}", m_session_id, state, applied_seek_request_id, m_latest_seek_request_id);
        return;
    }
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "RemotePlaybackManager({}): Server reports {} playing={} available={} time={} (mirror was {})", m_session_id, state, is_playing, to_underlying(available_data), current_time, m_state);
    if (state == Media::PlaybackState::Seeking)
        m_seek_timestamp = current_time;
    m_is_playing = is_playing;
    m_available_data = available_data;
    if (m_state == state)
        return;
    m_state = state;
    dispatch_state_change();
}

void RemotePlaybackManager::buffered_ranges_changed(Badge<Client>, Media::TimeRanges const& buffered_ranges)
{
    m_buffered_ranges = buffered_ranges;
    if (on_buffered_ranges_change)
        on_buffered_ranges_change();
}

void RemotePlaybackManager::error(Badge<Client>, Media::DecoderError error)
{
    dispatch_error(move(error));
}

void RemotePlaybackManager::video_resized(Badge<Client>, Media::VideoSinkHandle handle, Gfx::Size<u32> size)
{
    auto sink = m_video_sinks.get(handle);
    if (sink.has_value() && sink->on_resize)
        sink->on_resize(size);
}

void RemotePlaybackManager::video_edge_attached(Badge<Client>, Media::VideoSinkHandle handle, Media::PresentedFramePage presented_frame_page)
{
    if (auto sink = m_video_sinks.get(handle); sink.has_value())
        sink->presented_frame_page = move(presented_frame_page);
}

void RemotePlaybackManager::video_frame_pool_retired(Badge<Client>, Media::VideoSinkHandle handle, Media::VideoFramePoolID pool_id)
{
    if (auto sink = m_video_sinks.get(handle); sink.has_value())
        sink->slot_directory->notify_pool_retired(pool_id);
}

void RemotePlaybackManager::source_buffer_duration_received(Badge<Client>, u64 source_buffer_id, double duration)
{
    if (auto* source_buffer = find_source_buffer(source_buffer_id))
        source_buffer->duration_received({}, duration);
}

void RemotePlaybackManager::source_buffer_first_initialization_segment_received(Badge<Client>, u64 source_buffer_id, Vector<Media::Track> const& audio_tracks, Vector<Media::Track> const& video_tracks, Vector<Media::Track> const& text_tracks)
{
    if (auto* source_buffer = find_source_buffer(source_buffer_id))
        source_buffer->first_initialization_segment_received({}, audio_tracks, video_tracks, text_tracks);
}

void RemotePlaybackManager::source_buffer_coded_frames_processed(Badge<Client>, u64 source_buffer_id, AK::Duration group_end_timestamp)
{
    if (auto* source_buffer = find_source_buffer(source_buffer_id))
        source_buffer->coded_frames_processed({}, group_end_timestamp);
}

void RemotePlaybackManager::source_buffer_append_completed(Badge<Client>, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState state)
{
    if (auto* source_buffer = find_source_buffer(source_buffer_id))
        source_buffer->append_completed({}, append_generation, move(state));
}

void RemotePlaybackManager::source_buffer_append_failed(Badge<Client>, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState state)
{
    if (auto* source_buffer = find_source_buffer(source_buffer_id))
        source_buffer->append_failed({}, append_generation, move(state));
}

void RemotePlaybackManager::source_buffer_removal_completed(Badge<Client>, u64 source_buffer_id, Media::MediaSourceExtensions::PublishedState state)
{
    if (auto* source_buffer = find_source_buffer(source_buffer_id))
        source_buffer->removal_completed({}, move(state));
}

}
