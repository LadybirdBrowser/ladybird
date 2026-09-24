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

    PlaybackSession(ConnectionFromClient&, u64 id, bool audio_output_disabled);
    ~PlaybackSession();

    u64 id() const { return m_id; }
    Media::PlaybackManager& manager() { return *m_manager; }

    void seek(u64 seek_request_id, AK::Duration timestamp, Media::SeekMode);
    void reserve_video_sink(Media::Track const&, Media::VideoSinkHandle);

    void create_source_buffer(u64 source_buffer_id);
    void destroy_source_buffer(u64 source_buffer_id);
    void set_source_buffer_content_type(u64 source_buffer_id, StringView subtype);
    void add_to_source_buffer_input(u64 source_buffer_id, ReadonlyBytes);
    void run_source_buffer_append(u64 source_buffer_id, u64 append_generation);
    void remove_source_buffer_coded_frames(u64 source_buffer_id, AK::Duration start, AK::Duration end);
    void run_source_buffer_command(u64 source_buffer_id, Media::MediaSourceExtensions::Command);

private:
    enum class AppendOutcome {
        Pending,
        Completed,
        Failed,
    };

    struct SourceBuffer {
        NonnullRefPtr<Media::MediaSourceExtensions::SourceBufferProcessor> processor;
        // The data of the next append, which arrives in pieces small enough for one message each.
        ByteBuffer pending_append_data;
        AppendOutcome append_outcome { AppendOutcome::Pending };
        bool has_parser { false };
    };

    SourceBuffer* find_source_buffer(u64 source_buffer_id);
    void report_playback_state();

    ConnectionFromClient& m_connection;
    u64 m_id { 0 };
    NonnullOwnPtr<Media::PlaybackManager> m_manager;
    u64 m_applied_seek_request_id { 0 };

    // Tracks reported by the manager since the last metadata report, which delivers them together.
    Vector<Media::Track> m_tracks_added_since_last_metadata_report;

    HashMap<u64, SourceBuffer> m_source_buffers;
};

}
