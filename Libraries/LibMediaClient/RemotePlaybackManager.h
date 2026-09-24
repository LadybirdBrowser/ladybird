/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibGfx/Size.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaSourceExtensions/SourceBufferProcessor.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PlaybackStates/AvailableData.h>
#include <LibMedia/PlaybackStates/PlaybackState.h>
#include <LibMedia/SeekMode.h>
#include <LibMedia/TimeRanges.h>
#include <LibMedia/Track.h>
#include <LibMedia/VideoFrameHandle.h>
#include <LibMedia/VideoFramePool.h>
#include <LibMedia/VideoPresentation/PresentedFramePage.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaClient/Forward.h>

namespace MediaClient {

// The renderer's face of a playback session in the media server. It mirrors what the server reports and reads the
// playback clock through shared memory, so nothing a script observes waits on the server.
class RemotePlaybackManager : public Weakable<RemotePlaybackManager> {
    AK_MAKE_NONCOPYABLE(RemotePlaybackManager);
    AK_MAKE_NONMOVABLE(RemotePlaybackManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<RemotePlaybackManager> create(bool audio_output_disabled);
    ~RemotePlaybackManager();

    u64 session_id() const { return m_session_id; }
    Client* client() { return m_client.ptr(); }

    AK::Duration duration() const { return m_duration; }
    void set_duration(AK::Duration);
    AK::Duration current_time() const;

    Optional<AK::UnixDateTime> start_time_realtime() const { return m_start_time_realtime; }

    Vector<Media::Track> const& video_tracks() const { return m_video_tracks; }
    Vector<Media::Track> const& audio_tracks() const { return m_audio_tracks; }
    Optional<Media::Track> preferred_video_track() const { return m_preferred_video_track; }
    Optional<Media::Track> preferred_audio_track() const { return m_preferred_audio_track; }

    Media::VideoSinkHandle reserve_video_sink_handle(Media::Track const&);
    void disable_video_sink_by_handle(Media::VideoSinkHandle);
    void set_video_sink_ticking(Media::VideoSinkHandle, bool);
    void detach_video_sink(Media::VideoSinkHandle);
    void set_video_resize_handler(Media::VideoSinkHandle, Function<void(Gfx::Size<u32>)>);
    RefPtr<Media::VideoFrame> current_presented_frame(Media::VideoSinkHandle);

    void enable_an_audio_track(Media::Track const&);
    void disable_an_audio_track(Media::Track const&);

    void add_media_source(RemoteMediaStream&);

    void start();
    void play();
    void pause();
    void seek(AK::Duration timestamp, Media::SeekMode);

    bool is_playing() const { return m_is_playing; }
    Media::PlaybackState state() const { return m_state; }
    Media::AvailableData available_data() const { return m_available_data; }
    Media::TimeRanges const& buffered_time_ranges() const { return m_buffered_ranges; }

    void set_volume(double);
    void set_playback_rate(float);

    Function<void()> on_metadata_parsed;
    Function<void(Media::Track const&)> on_track_added;
    Function<void()> on_playback_state_change;
    Function<void(AK::Duration)> on_duration_change;
    Function<void()> on_buffered_ranges_change;
    Function<void(Media::DecoderError&&)> on_error;

    void register_source_buffer(Badge<RemoteSourceBuffer>, RemoteSourceBuffer&);
    void unregister_source_buffer(Badge<RemoteSourceBuffer>, RemoteSourceBuffer&);

    void connection_lost(Badge<Client>) { handle_connection_lost(); }
    void clock_changed(Badge<Client>, Media::MediaTimeReader);
    void metadata_parsed(Badge<Client>, Vector<Media::Track> const& audio_tracks, Vector<Media::Track> const& video_tracks, Optional<Media::Track> preferred_audio_track, Optional<Media::Track> preferred_video_track, Optional<AK::UnixDateTime> start_time_realtime);
    void duration_changed(Badge<Client>, AK::Duration);
    void state_changed(Badge<Client>, u64 applied_seek_request_id, Media::PlaybackState, bool is_playing, Media::AvailableData, AK::Duration current_time);
    void buffered_ranges_changed(Badge<Client>, Media::TimeRanges const&);
    void error(Badge<Client>, Media::DecoderError);
    void video_resized(Badge<Client>, Media::VideoSinkHandle, Gfx::Size<u32>);
    void video_edge_attached(Badge<Client>, Media::VideoSinkHandle, Media::PresentedFramePage);
    void video_frame_pool_retired(Badge<Client>, Media::VideoSinkHandle, Media::VideoFramePoolID);
    void source_buffer_duration_received(Badge<Client>, u64 source_buffer_id, double duration);
    void source_buffer_first_initialization_segment_received(Badge<Client>, u64 source_buffer_id, Vector<Media::Track> const& audio_tracks, Vector<Media::Track> const& video_tracks, Vector<Media::Track> const& text_tracks);
    void source_buffer_coded_frames_processed(Badge<Client>, u64 source_buffer_id, AK::Duration group_end_timestamp);
    void source_buffer_append_completed(Badge<Client>, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState);
    void source_buffer_append_failed(Badge<Client>, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState);
    void source_buffer_removal_completed(Badge<Client>, u64 source_buffer_id, Media::MediaSourceExtensions::PublishedState);

private:
    struct VideoSink {
        Optional<Media::PresentedFramePage> presented_frame_page;
        NonnullRefPtr<Media::VideoFrameSlotDirectory> slot_directory { Media::VideoFrameSlotDirectory::create() };
        Function<void(Gfx::Size<u32>)> on_resize;
    };

    RemotePlaybackManager(RefPtr<Client>, u64 session_id);

    bool can_send() const;
    void handle_connection_lost();
    void dispatch_error(Media::DecoderError&&);
    RemoteSourceBuffer* find_source_buffer(u64 source_buffer_id);
    void dispatch_state_change() const;

    RefPtr<Client> m_client;
    u64 m_session_id { 0 };

    Optional<Media::MediaTimeReader> m_time_reader;
    AK::Duration m_duration;
    bool m_duration_was_provided { false };
    Optional<AK::UnixDateTime> m_start_time_realtime;
    bool m_is_in_error_state { false };
    Vector<Media::Track> m_audio_tracks;
    Vector<Media::Track> m_video_tracks;
    Optional<Media::Track> m_preferred_audio_track;
    Optional<Media::Track> m_preferred_video_track;

    Media::PlaybackState m_state { Media::PlaybackState::Starting };
    bool m_is_playing { false };
    Media::AvailableData m_available_data { Media::AvailableData::None };
    Media::TimeRanges m_buffered_ranges;

    // A seek reports its target as the current time until the server has chosen where it lands. State reported
    // before the server applied the latest seek request is stale, and is ignored.
    u64 m_latest_seek_request_id { 0 };
    AK::Duration m_seek_timestamp;

    HashMap<Media::VideoSinkHandle, VideoSink> m_video_sinks;
    HashMap<u64, RemoteSourceBuffer*> m_source_buffers;
};

}
