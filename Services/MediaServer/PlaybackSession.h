/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/MediaSourceExtensions/SourceBufferProcessor.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Track.h>
#include <LibMedia/VideoSinkHandle.h>
#include <MediaServer/Forward.h>

namespace MediaServer {

// One media element's playback, hosted for the renderer that owns the element. Everything the renderer's proxy
// mirrors is reported to it through the connection as it changes.
class PlaybackSession {
    AK_MAKE_NONCOPYABLE(PlaybackSession);
    AK_MAKE_NONMOVABLE(PlaybackSession);

public:
    AK_ALLOC_WITH_KMALLOC;

    PlaybackSession(ConnectionFromClient&, u64 id, Media::AudioOutput);
    ~PlaybackSession();

    u64 id() const { return m_id; }
    Media::PlaybackManager& manager() { return *m_manager; }

    void add_media_stream_source(u64 stream_id, NonnullRefPtr<Media::MediaStream> const&);

    void seek(u64 seek_request_id, AK::Duration timestamp, Media::SeekMode);
    void set_audio_track_enabled(u64 seek_request_id, Media::Track const&, bool enabled, bool resume_ended_playback);
    void reserve_video_sink(u64 seek_request_id, Media::Track const&, Media::VideoSinkHandle, bool resume_ended_playback);
    void disable_video_sink(u64 seek_request_id, Media::VideoSinkHandle);

    void create_source_buffer(u64 source_buffer_id);
    void destroy_source_buffer(u64 source_buffer_id);
    void set_source_buffer_content_type(u64 source_buffer_id, StringView subtype);
    void add_to_source_buffer_input(u64 source_buffer_id, ReadonlyBytes);
    void run_source_buffer_append(u64 source_buffer_id, u64 append_generation);
    void remove_source_buffer_coded_frames(u64 source_buffer_id, AK::Duration start, AK::Duration end);
    void run_source_buffer_coded_frame_eviction(u64 source_buffer_id, size_t new_data_size);
    Optional<Media::MediaSourceExtensions::PublishedState> source_buffer_published_state(u64 source_buffer_id);
    void run_source_buffer_command(u64 source_buffer_id, Media::MediaSourceExtensions::Command);

private:
    enum class AppendOutcome {
        Pending,
        Completed,
        Failed,
    };

    enum class TrackAdditionOutcome {
        Added,
        Failed,
    };

    struct HeldMessage {
        // Messages that report an append are dropped if that append fails.
        Optional<u64> append_generation;
        Function<void()> send;
    };

    struct SourceBuffer {
        NonnullRefPtr<Media::MediaSourceExtensions::SourceBufferProcessor> processor;
        // The data of the next append, which arrives in pieces small enough for one message each.
        ByteBuffer pending_append_data;
        AppendOutcome append_outcome { AppendOutcome::Pending };
        u64 append_generation { 0 };
        bool has_parser { false };

        // Until the manager has added the first initialization segment's tracks, the renderer's messages wait, so
        // that it knows the tracks before it processes the segment.
        bool is_adding_tracks { false };
        u64 first_initialization_segment_append_generation { 0 };
        Vector<HeldMessage> held_messages {};
    };

    struct MediaStreamSource {
        NonnullRefPtr<Media::Demuxer> demuxer;
        Media::DemuxerScanState reported_scan_state;
    };

    void report_media_stream_scan_states();

    SourceBuffer* find_source_buffer(u64 source_buffer_id);
    Media::MediaSourceExtensions::PublishedState published_state_for_renderer(SourceBuffer const&) const;
    void add_source_buffer_demuxer(u64 source_buffer_id, NonnullRefPtr<Media::Demuxer> const&);
    void finish_source_buffer_track_addition(u64 source_buffer_id, TrackAdditionOutcome);
    static void send_or_hold(SourceBuffer&, Optional<u64> append_generation, Function<void()>);
    void report_playback_state();

    ConnectionFromClient& m_connection;
    u64 m_id { 0 };
    NonnullOwnPtr<Media::PlaybackManager> m_manager;
    u64 m_applied_seek_request_id { 0 };

    HashMap<u64, MediaStreamSource> m_media_stream_sources;
    HashMap<u64, SourceBuffer> m_source_buffers;
};

}
