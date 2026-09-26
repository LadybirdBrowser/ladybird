/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/TransportHandle.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/MediaSupport.h>
#include <LibMediaClient/Forward.h>
#include <MediaServer/MediaClientEndpoint.h>
#include <MediaServer/MediaServerEndpoint.h>

namespace MediaClient {

class Client final
    : public IPC::ConnectionToServer<MediaClientEndpoint, MediaServerEndpoint>
    , public MediaClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::MediaServer::InitTransport;
    using TransportFactory = Function<ErrorOr<NonnullOwnPtr<IPC::Transport>>()>;

    // Installed by the process that reaches the Browser, which spawns the process's MediaServer and brokers the
    // connection to it on request.
    static void set_transport_factory(TransportFactory);

    // The process's connection to its MediaServer, established on first use and dropped once it has had no
    // sessions for a while. Each connection is a fresh server process, so a holder of state tied to one keeps
    // its generation to notice a replacement.
    static ErrorOr<NonnullRefPtr<Client>> acquire();

    explicit Client(NonnullOwnPtr<IPC::Transport>);
    virtual ~Client() override;

    u64 generation() const { return m_generation; }

    Function<void()> on_death;

    ErrorOr<IPC::TransportHandle> create_video_presentation_channel();

    // The synchronous queries serve APIs that must answer on the spot; everything else asks through a promise.
    Media::MediaSupportInfo query_file_media_support(StringView type, StringView subtype, Optional<String> codecs_parameter);
    Optional<Media::DecoderCapabilities> query_decoder_capabilities(StringView codecs_parameter);
    using FileMediaSupportPromise = Core::Promise<Media::MediaSupportInfo>;
    NonnullRefPtr<FileMediaSupportPromise> request_file_media_support(String const& type, String const& subtype, Optional<String const&> codecs_parameter);
    using DecoderCapabilitiesPromise = Core::Promise<Optional<Media::DecoderCapabilities>>;
    NonnullRefPtr<DecoderCapabilitiesPromise> request_decoder_capabilities(StringView codecs_parameter);

    // Linear PCM decoded by the server, one channel after another in shared memory.
    struct DecodedAudioData {
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        u64 frame_count { 0 };
        Core::AnonymousBuffer planar_samples;

        ReadonlySpan<float> channel(u32 index) const { return { planar_samples.data<float>() + static_cast<size_t>(index) * frame_count, frame_count }; }
    };
    using DecodeAudioDataCallback = Function<void(Media::DecoderErrorOr<DecodedAudioData>)>;
    // The callback runs on this event loop once the server answers, or at once with an error if it cannot be asked.
    void decode_audio_data(ReadonlyBytes, u32 output_sample_rate, DecodeAudioDataCallback);

    u64 allocate_id();

    void register_media_stream(Badge<RemoteMediaStream>, RemoteMediaStream&);
    void unregister_media_stream(Badge<RemoteMediaStream>, RemoteMediaStream&);
    void register_playback_manager(Badge<RemotePlaybackManager>, RemotePlaybackManager&);
    void unregister_playback_manager(Badge<RemotePlaybackManager>, RemotePlaybackManager&);

private:
    virtual void die() override;

    virtual void file_media_support_reported(u64 request_id, Media::MediaSupportInfo info) override;
    virtual void decoder_capabilities_reported(u64 request_id, Optional<Media::DecoderCapabilities> capabilities) override;
    virtual void audio_data_decoded(u64 request_id, u32 sample_rate, u32 channel_count, u64 frame_count, Core::AnonymousBuffer planar_samples) override;
    virtual void audio_data_decode_failed(u64 request_id, Media::DecoderError error) override;

    virtual void media_stream_data_requested(u64 stream_id, Optional<u64> offset) override;

    virtual void playback_session_clock_changed(u64 session_id, Media::MediaTimeReader time_reader) override;
    virtual void playback_session_metadata_parsed(u64 session_id, Vector<Media::Track> audio_tracks, Vector<Media::Track> video_tracks, Optional<Media::Track> preferred_audio_track, Optional<Media::Track> preferred_video_track, Optional<UnixDateTime> start_time_realtime) override;
    virtual void playback_session_duration_changed(u64 session_id, AK::Duration duration) override;
    virtual void playback_session_state_changed(u64 session_id, u64 applied_seek_request_id, Media::PlaybackState state, bool is_playing, Media::AvailableData available_data, AK::Duration current_time) override;
    virtual void playback_session_buffered_ranges_changed(u64 session_id, Media::TimeRanges buffered_ranges) override;
    virtual void playback_session_error(u64 session_id, Media::DecoderError error) override;
    virtual void playback_session_video_resized(u64 session_id, Media::VideoSinkHandle handle, u32 width, u32 height) override;
    virtual void playback_session_video_edge_attached(u64 session_id, Media::VideoSinkHandle handle, Media::PresentedFramePage presented_frame_page) override;
    virtual void playback_session_video_frame_pool_retired(u64 session_id, Media::VideoSinkHandle handle, Media::VideoFramePoolID pool_id) override;

    virtual void source_buffer_duration_received(u64 session_id, u64 source_buffer_id, double duration) override;
    virtual void source_buffer_first_initialization_segment_received(u64 session_id, u64 source_buffer_id, Vector<Media::Track> audio_tracks, Vector<Media::Track> video_tracks, Vector<Media::Track> text_tracks) override;
    virtual void source_buffer_coded_frames_processed(u64 session_id, u64 source_buffer_id, AK::Duration group_end_timestamp) override;
    virtual void source_buffer_append_completed(u64 session_id, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState state) override;
    virtual void source_buffer_append_failed(u64 session_id, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState state) override;
    virtual void source_buffer_removal_completed(u64 session_id, u64 source_buffer_id, Media::MediaSourceExtensions::PublishedState state) override;

    RemotePlaybackManager* find_playback_manager(u64 session_id);
    void update_idle_timer();
    void verify_event_loop() const;

    Core::EventLoop* m_creation_event_loop { &Core::EventLoop::current() };
    u64 m_generation { 0 };
    u64 m_next_id { 1 };

    HashMap<u64, DecodeAudioDataCallback> m_pending_audio_data_decodes;
    HashMap<u64, NonnullRefPtr<FileMediaSupportPromise>> m_pending_file_media_support_requests;
    HashMap<u64, NonnullRefPtr<DecoderCapabilitiesPromise>> m_pending_decoder_capabilities_requests;
    HashMap<u64, RemoteMediaStream*> m_media_streams;
    HashMap<u64, RemotePlaybackManager*> m_playback_managers;
    RefPtr<Core::Timer> m_idle_timer;
};

}
