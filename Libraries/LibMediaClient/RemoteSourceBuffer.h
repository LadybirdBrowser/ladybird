/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/MediaSourceExtensions/SourceBufferProcessor.h>
#include <LibMedia/Track.h>
#include <LibMediaClient/Forward.h>

namespace MediaClient {

// The renderer's face of a SourceBuffer's processor in the media server. Mutations go to the server in order; the
// state the SourceBuffer reads synchronously is a mirror that the server refreshes with each append or removal it
// completes, and that setters update at once, so a read never lags the updating attribute.
class RemoteSourceBuffer : public RefCounted<RemoteSourceBuffer> {
public:
    static NonnullRefPtr<RemoteSourceBuffer> create(RemotePlaybackManager&);
    ~RemoteSourceBuffer();

    u64 id() const { return m_id; }

    Media::MediaSourceExtensions::PublishedState const& published_state() const { return m_published_state; }

    void set_content_type_subtype(StringView subtype);

    // An append is held here until the buffer append algorithm's task sends it, so that an abort before that task
    // runs drops it without the server ever seeing it.
    void enqueue_append(ByteBuffer);
    void abandon_append();
    void send_pending_append(u64 append_generation);

    void reset_parser_state();
    void remove_coded_frames(AK::Duration start, AK::Duration end);
    void set_mode(Media::MediaSourceExtensions::AppendMode);
    void set_timestamp_offset(AK::Duration);
    void set_generate_timestamps_flag(bool);
    void set_pending_initialization_segment_for_change_type_flag(bool);
    void set_reached_end_of_stream(bool);

    Function<void(double duration)> on_duration_received;
    Function<void(Vector<Media::Track> audio_tracks, Vector<Media::Track> video_tracks, Vector<Media::Track> text_tracks)> on_first_initialization_segment_received;
    Function<void(AK::Duration group_end_timestamp)> on_coded_frames_processed;
    Function<void(u64 append_generation)> on_append_completed;
    Function<void(u64 append_generation)> on_append_failed;
    Function<void()> on_removal_completed;

    void playback_manager_destroyed(Badge<RemotePlaybackManager>);
    void connection_lost(Badge<RemotePlaybackManager>);
    void duration_received(Badge<RemotePlaybackManager>, double duration);
    void first_initialization_segment_received(Badge<RemotePlaybackManager>, Vector<Media::Track> audio_tracks, Vector<Media::Track> video_tracks, Vector<Media::Track> text_tracks);
    void coded_frames_processed(Badge<RemotePlaybackManager>, AK::Duration group_end_timestamp);
    void append_completed(Badge<RemotePlaybackManager>, u64 append_generation, Media::MediaSourceExtensions::PublishedState);
    void append_failed(Badge<RemotePlaybackManager>, u64 append_generation, Media::MediaSourceExtensions::PublishedState);
    void removal_completed(Badge<RemotePlaybackManager>, Media::MediaSourceExtensions::PublishedState);

private:
    RemoteSourceBuffer(RemotePlaybackManager&, RefPtr<Client>, u64 session_id, u64 id);

    bool can_send() const;

    RemotePlaybackManager* m_playback_manager { nullptr };
    RefPtr<Client> m_client;
    u64 m_session_id { 0 };
    u64 m_id { 0 };

    Media::MediaSourceExtensions::PublishedState m_published_state;
    Optional<ByteBuffer> m_pending_append;
    Optional<u64> m_append_generation_awaiting_outcome;
};

}
