/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibMedia/MediaSourceExtensions/ISOBMFFByteStreamParser.h>
#include <LibMedia/MediaSourceExtensions/SourceBufferDemuxer.h>
#include <LibMedia/MediaSourceExtensions/WebMByteStreamParser.h>
#include <MediaServer/ConnectionFromClient.h>
#include <MediaServer/PlaybackSession.h>

namespace MediaServer {

namespace Commands = Media::MediaSourceExtensions::Commands;

PlaybackSession::PlaybackSession(ConnectionFromClient& connection, u64 id, Media::AudioOutput audio_output)
    : m_connection(connection)
    , m_id(id)
    , m_manager(Media::PlaybackManager::create())
{
    m_manager->set_audio_output(audio_output);

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

void PlaybackSession::add_media_stream_source(u64 stream_id, NonnullRefPtr<Media::MediaStream> const& stream)
{
    m_manager->add_media_source(stream)
        ->when_resolved([this, stream_id](Media::PlaybackManager::AddedTracks& added_tracks) {
            m_connection.async_playback_session_media_source_added(m_id, stream_id, move(added_tracks.audio_tracks), move(added_tracks.video_tracks), m_manager->preferred_audio_track(), m_manager->preferred_video_track(), m_manager->start_time_realtime());
        })
        .when_rejected([this, stream_id](Media::DecoderError& error) {
            m_connection.async_playback_session_media_stream_source_failed(m_id, stream_id, move(error));
        });
}

void PlaybackSession::seek(u64 seek_request_id, AK::Duration timestamp, Media::SeekMode mode)
{
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "PlaybackSession({}): Applying seek {} to {} ({})", m_id, seek_request_id, timestamp, mode);
    m_applied_seek_request_id = seek_request_id;
    m_manager->seek(timestamp, mode);
    // Seeking again while seeking changes no state, so the renderer learns the chosen timestamp from here.
    report_playback_state();
}

void PlaybackSession::set_audio_track_enabled(u64 seek_request_id, Media::Track const& track, bool enabled, bool resume_ended_playback)
{
    m_applied_seek_request_id = seek_request_id;
    if (m_manager->audio_tracks().contains_slow(track) && m_manager->track_is_enabled(track) != enabled) {
        if (enabled)
            m_manager->enable_an_audio_track(track, resume_ended_playback ? Media::PlaybackManager::ResumeEndedPlayback::Yes : Media::PlaybackManager::ResumeEndedPlayback::No);
        else
            m_manager->disable_an_audio_track(track);
    }
    // Reports emitted before the change carry the previous id, so the renderer relearns the state from here.
    report_playback_state();
}

void PlaybackSession::reserve_video_sink(u64 seek_request_id, Media::Track const& track, Media::VideoSinkHandle handle, bool resume_ended_playback)
{
    m_applied_seek_request_id = seek_request_id;
    if (m_manager->video_tracks().contains_slow(track)) {
        m_manager->reserve_video_sink_handle(track, handle, resume_ended_playback ? Media::PlaybackManager::ResumeEndedPlayback::Yes : Media::PlaybackManager::ResumeEndedPlayback::No);
        m_manager->set_video_resize_handler(handle, [this, handle](Gfx::Size<u32> size) {
            m_connection.async_playback_session_video_resized(m_id, handle, size.width(), size.height());
        });
    }
    report_playback_state();
}

void PlaybackSession::disable_video_sink(u64 seek_request_id, Media::VideoSinkHandle handle)
{
    m_applied_seek_request_id = seek_request_id;
    m_manager->disable_video_sink_by_handle(handle);
    report_playback_state();
}

void PlaybackSession::add_source_buffer_demuxer(u64 source_buffer_id, NonnullRefPtr<Media::Demuxer> const& demuxer)
{
    m_manager->add_media_source(demuxer)
        ->when_resolved([this, source_buffer_id](Media::PlaybackManager::AddedTracks& added_tracks) {
            m_connection.async_playback_session_media_source_added(m_id, {}, move(added_tracks.audio_tracks), move(added_tracks.video_tracks), m_manager->preferred_audio_track(), m_manager->preferred_video_track(), m_manager->start_time_realtime());
            finish_source_buffer_track_addition(source_buffer_id, TrackAdditionOutcome::Added);
        })
        .when_rejected([this, source_buffer_id](Media::DecoderError& error) {
            dbgln("PlaybackSession({}): Could not add the tracks of source buffer {}: {}", m_id, source_buffer_id, error.description());
            finish_source_buffer_track_addition(source_buffer_id, TrackAdditionOutcome::Failed);
        });
}

void PlaybackSession::finish_source_buffer_track_addition(u64 source_buffer_id, TrackAdditionOutcome outcome)
{
    auto* source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;
    VERIFY(source_buffer->is_adding_tracks);
    source_buffer->is_adding_tracks = false;

    auto held_messages = move(source_buffer->held_messages);
    if (outcome == TrackAdditionOutcome::Failed) {
        // The segment's tracks cannot be played, so the append that delivered it fails, and nothing it produced is
        // reported. The renderer's append error algorithm resets the parser.
        auto failed_append_generation = source_buffer->first_initialization_segment_append_generation;
        held_messages.remove_all_matching([&](auto const& message) { return message.append_generation == failed_append_generation; });
        m_connection.async_source_buffer_append_failed(m_id, source_buffer_id, failed_append_generation, source_buffer->processor->published_state());
    }
    for (auto& message : held_messages)
        message.send();
}

void PlaybackSession::send_or_hold(SourceBuffer& source_buffer, Optional<u64> append_generation, Function<void()> send)
{
    if (!source_buffer.is_adding_tracks) {
        send();
        return;
    }
    source_buffer.held_messages.append({ append_generation, move(send) });
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
        auto* source_buffer = find_source_buffer(source_buffer_id);
        if (!source_buffer)
            return;
        source_buffer->first_initialization_segment_append_generation = source_buffer->append_generation;

        if (!segment.audio_tracks.is_empty() || !segment.video_tracks.is_empty()) {
            source_buffer->is_adding_tracks = true;
            add_source_buffer_demuxer(source_buffer_id, segment.demuxer);
        }
        send_or_hold(*source_buffer, source_buffer->append_generation, [this, source_buffer_id, audio_tracks = move(segment.audio_tracks), video_tracks = move(segment.video_tracks), text_tracks = move(segment.text_tracks)] mutable {
            m_connection.async_source_buffer_first_initialization_segment_received(m_id, source_buffer_id, move(audio_tracks), move(video_tracks), move(text_tracks));
        });
    });
    processor->set_append_error_callback([this, source_buffer_id] {
        if (auto* source_buffer = find_source_buffer(source_buffer_id))
            source_buffer->append_outcome = AppendOutcome::Failed;
    });
    processor->set_coded_frame_processing_done_callback([this, source_buffer_id](AK::Duration group_end_timestamp) {
        auto* source_buffer = find_source_buffer(source_buffer_id);
        if (!source_buffer)
            return;
        send_or_hold(*source_buffer, source_buffer->append_generation, [this, source_buffer_id, group_end_timestamp] {
            m_connection.async_source_buffer_coded_frames_processed(m_id, source_buffer_id, group_end_timestamp);
        });
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
    source_buffer->append_generation = append_generation;
    source_buffer->processor->run(Commands::BufferAppend { move(data) });

    // The callbacks run within the append and may have destroyed this source buffer's entry.
    source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;
    auto state = source_buffer->processor->published_state();
    switch (source_buffer->append_outcome) {
    case AppendOutcome::Completed:
        send_or_hold(*source_buffer, append_generation, [this, source_buffer_id, append_generation, state = move(state)] mutable {
            m_connection.async_source_buffer_append_completed(m_id, source_buffer_id, append_generation, move(state));
        });
        break;
    case AppendOutcome::Failed:
        send_or_hold(*source_buffer, append_generation, [this, source_buffer_id, append_generation, state = move(state)] mutable {
            m_connection.async_source_buffer_append_failed(m_id, source_buffer_id, append_generation, move(state));
        });
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
    send_or_hold(*source_buffer, {}, [this, source_buffer_id, state = source_buffer->processor->published_state()] mutable {
        m_connection.async_source_buffer_removal_completed(m_id, source_buffer_id, move(state));
    });
}

void PlaybackSession::run_source_buffer_command(u64 source_buffer_id, Media::MediaSourceExtensions::Command command)
{
    auto* source_buffer = find_source_buffer(source_buffer_id);
    if (!source_buffer)
        return;
    source_buffer->processor->run(move(command));
}

}
