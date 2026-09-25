/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibMedia/MediaSourceExtensions/ISOBMFFByteStreamParser.h>
#include <LibMedia/MediaSourceExtensions/TrackBufferDemuxer.h>
#include <LibMedia/MediaSourceExtensions/WebMByteStreamParser.h>
#include <MediaServer/ConnectionFromClient.h>
#include <MediaServer/PlaybackSession.h>

namespace MediaServer {

namespace Commands = Media::MediaSourceExtensions::Commands;

PlaybackSession::PlaybackSession(ConnectionFromClient& connection, u64 id, bool audio_output_disabled)
    : m_connection(connection)
    , m_id(id)
    , m_manager(Media::PlaybackManager::create())
{
    m_manager->set_audio_output_disabled(audio_output_disabled);

    m_manager->on_track_added = [this](Media::Track const& track) {
        m_tracks_added_since_last_metadata_report.append(track);
    };
    m_manager->on_metadata_parsed = [this] {
        Vector<Media::Track> audio_tracks;
        Vector<Media::Track> video_tracks;
        for (auto const& track : m_tracks_added_since_last_metadata_report) {
            if (track.type() == Media::TrackType::Audio)
                audio_tracks.append(track);
            else if (track.type() == Media::TrackType::Video)
                video_tracks.append(track);
        }
        m_tracks_added_since_last_metadata_report.clear();
        m_connection.async_playback_session_metadata_parsed(m_id, move(audio_tracks), move(video_tracks), m_manager->preferred_audio_track(), m_manager->preferred_video_track(), m_manager->start_time_realtime());
    };
    m_manager->on_playback_state_change = [this] {
        report_playback_state();
    };
    m_manager->on_duration_change = [this](AK::Duration duration) {
        m_connection.async_playback_session_duration_changed(m_id, duration);
    };
    m_manager->on_buffered_ranges_change = [this] {
        m_connection.async_playback_session_buffered_ranges_changed(m_id, m_manager->buffered_time_ranges());
    };
    m_manager->on_error = [this](Media::DecoderError&& error) {
        m_connection.async_playback_session_error(m_id, move(error));
    };
    m_manager->set_host_hooks({
        .on_clock_changed = [this](Media::MediaTimeReader const& time_reader) { m_connection.async_playback_session_clock_changed(m_id, time_reader); },
        .on_video_edge_attached = [this](Media::VideoSinkHandle handle, Media::PresentedFramePage const& presented_frame_page) { m_connection.async_playback_session_video_edge_attached(m_id, handle, presented_frame_page); },
        .on_video_frame_pool_retired = [this](Media::VideoSinkHandle handle, Media::VideoFramePoolID pool_id) { m_connection.async_playback_session_video_frame_pool_retired(m_id, handle, pool_id); },
    });

    m_connection.async_playback_session_clock_changed(m_id, m_manager->time_reader());
}

PlaybackSession::~PlaybackSession() = default;

void PlaybackSession::report_playback_state()
{
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "PlaybackSession({}): Reporting {} playing={} available={} time={} after seek {}", m_id, m_manager->state(), m_manager->is_playing(), to_underlying(m_manager->available_data()), m_manager->current_time(), m_applied_seek_request_id);
    m_connection.async_playback_session_state_changed(m_id, m_applied_seek_request_id, m_manager->state(), m_manager->is_playing(), m_manager->available_data(), m_manager->current_time());
}

void PlaybackSession::seek(u64 seek_request_id, AK::Duration timestamp, Media::SeekMode mode)
{
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "PlaybackSession({}): Applying seek {} to {} ({})", m_id, seek_request_id, timestamp, mode);
    m_applied_seek_request_id = seek_request_id;
    m_manager->seek(timestamp, mode);
    // Seeking again while seeking changes no state, so the renderer learns the chosen timestamp from here.
    report_playback_state();
}

void PlaybackSession::reserve_video_sink(Media::Track const& track, Media::VideoSinkHandle handle)
{
    if (!m_manager->video_tracks().contains_slow(track))
        return;
    m_manager->reserve_video_sink_handle(track, handle);
    m_manager->set_video_resize_handler(handle, [this, handle](Gfx::Size<u32> size) {
        m_connection.async_playback_session_video_resized(m_id, handle, size.width(), size.height());
    });
}

PlaybackSession::SourceBuffer* PlaybackSession::find_source_buffer(u64 source_buffer_id)
{
    auto it = m_source_buffers.find(source_buffer_id);
    if (it == m_source_buffers.end())
        return nullptr;
    return &it->value;
}

void PlaybackSession::create_source_buffer(u64 source_buffer_id)
{
    if (m_source_buffers.contains(source_buffer_id)) {
        m_connection.did_misbehave("Duplicate source buffer ID");
        return;
    }

    auto processor = adopt_ref(*new Media::MediaSourceExtensions::SourceBufferProcessor());
    processor->set_duration_change_callback([this, source_buffer_id](double duration) {
        m_connection.async_source_buffer_duration_received(m_id, source_buffer_id, duration);
    });
    processor->set_first_initialization_segment_callback([this, source_buffer_id](Media::MediaSourceExtensions::InitializationSegmentData&& segment) {
        Vector<Media::Track> audio_tracks;
        Vector<Media::Track> video_tracks;
        Vector<Media::Track> text_tracks;
        for (auto const& track_data : segment.audio_tracks) {
            audio_tracks.append(track_data.track);
            m_manager->add_media_source(track_data.demuxer);
        }
        for (auto const& track_data : segment.video_tracks) {
            video_tracks.append(track_data.track);
            m_manager->add_media_source(track_data.demuxer);
        }
        for (auto const& track_data : segment.text_tracks)
            text_tracks.append(track_data.track);
        m_connection.async_source_buffer_first_initialization_segment_received(m_id, source_buffer_id, move(audio_tracks), move(video_tracks), move(text_tracks));
    });
    processor->set_append_error_callback([this, source_buffer_id] {
        if (auto* source_buffer = find_source_buffer(source_buffer_id))
            source_buffer->append_outcome = AppendOutcome::Failed;
    });
    processor->set_coded_frame_processing_done_callback([this, source_buffer_id](AK::Duration group_end_timestamp) {
        m_connection.async_source_buffer_coded_frames_processed(m_id, source_buffer_id, group_end_timestamp);
    });
    processor->set_append_done_callback([this, source_buffer_id] {
        if (auto* source_buffer = find_source_buffer(source_buffer_id))
            source_buffer->append_outcome = AppendOutcome::Completed;
    });

    m_source_buffers.set(source_buffer_id, SourceBuffer { .processor = move(processor), .pending_append_data = {}, .append_outcome = AppendOutcome::Pending });
}

void PlaybackSession::destroy_source_buffer(u64 source_buffer_id)
{
    m_source_buffers.remove(source_buffer_id);
}

void PlaybackSession::set_source_buffer_content_type(u64 source_buffer_id, StringView subtype)
{
    auto* source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;

    OwnPtr<Media::MediaSourceExtensions::ByteStreamParser> parser;
    if (subtype == "webm"sv)
        parser = make<Media::MediaSourceExtensions::WebMByteStreamParser>();
    else if (subtype == "mp4"sv)
        parser = make<Media::MediaSourceExtensions::ISOBMFFByteStreamParser>();
    if (!parser) {
        m_connection.did_misbehave("Unsupported source buffer content type");
        return;
    }
    source_buffer->processor->run(Commands::SetParser { parser.release_nonnull() });
    source_buffer->has_parser = true;
}

void PlaybackSession::add_to_source_buffer_input(u64 source_buffer_id, ReadonlyBytes data)
{
    if (auto* source_buffer = find_source_buffer(source_buffer_id))
        source_buffer->pending_append_data.append(data);
}

void PlaybackSession::run_source_buffer_append(u64 source_buffer_id, u64 append_generation)
{
    auto* source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;
    if (!source_buffer->has_parser) {
        m_connection.did_misbehave("Appending to a source buffer that has no content type");
        return;
    }
    auto data = move(source_buffer->pending_append_data);

    // The renderer checked the buffer full flag it last saw; eviction runs here so that it uses the current playback
    // position instead of one the renderer had to ask for.
    source_buffer->processor->run(Commands::CodedFrameEviction { data.size(), m_manager->current_time() });

    source_buffer->append_outcome = AppendOutcome::Pending;
    source_buffer->processor->run(Commands::BufferAppend { move(data) });

    // The callbacks run within the append and may have destroyed this source buffer's entry.
    source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;
    switch (source_buffer->append_outcome) {
    case AppendOutcome::Completed:
        m_connection.async_source_buffer_append_completed(m_id, source_buffer_id, append_generation, source_buffer->processor->published_state());
        break;
    case AppendOutcome::Failed:
        m_connection.async_source_buffer_append_failed(m_id, source_buffer_id, append_generation, source_buffer->processor->published_state());
        break;
    case AppendOutcome::Pending:
        VERIFY_NOT_REACHED();
    }
}

void PlaybackSession::remove_source_buffer_coded_frames(u64 source_buffer_id, AK::Duration start, AK::Duration end)
{
    auto* source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;
    source_buffer->processor->run(Commands::CodedFrameRemoval { start, end });
    m_connection.async_source_buffer_removal_completed(m_id, source_buffer_id, source_buffer->processor->published_state());
}

void PlaybackSession::run_source_buffer_command(u64 source_buffer_id, Media::MediaSourceExtensions::Command command)
{
    auto* source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;
    source_buffer->processor->run(move(command));
}

}
