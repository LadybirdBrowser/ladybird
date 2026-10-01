/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/TransportHandle.h>
#include <LibMedia/Forward.h>
#include <LibMedia/VideoPresentation/VideoPresentationServerConnection.h>
#include <MediaServer/Forward.h>
#include <MediaServer/MediaClientEndpoint.h>
#include <MediaServer/MediaServerEndpoint.h>

namespace MediaServer {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<MediaClientEndpoint, MediaServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    // The Browser holds the controller connection and brokers the one renderer connection. The server serves that
    // one renderer, so it exits when either connection goes away.
    enum class Role {
        Controller,
        Renderer,
    };

    ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, Role);
    ~ConnectionFromClient() override;

    virtual void die() override;

private:
    virtual Messages::MediaServer::InitTransportResponse init_transport(int peer_pid) override;
    virtual Messages::MediaServer::ConnectNewClientResponse connect_new_client() override;

    virtual Messages::MediaServer::CreateVideoPresentationChannelResponse create_video_presentation_channel() override;

    virtual Messages::MediaServer::QueryFileMediaSupportResponse query_file_media_support(String type, String subtype, Optional<String> codecs_parameter) override;
    virtual Messages::MediaServer::QueryDecoderCapabilitiesResponse query_decoder_capabilities(String codecs_parameter) override;
    virtual void request_file_media_support(u64 request_id, String type, String subtype, Optional<String> codecs_parameter) override;
    virtual void request_decoder_capabilities(u64 request_id, String codecs_parameter) override;
    virtual void decode_audio_data(u64 request_id, Core::AnonymousBuffer data, u32 output_sample_rate) override;

    virtual void create_media_stream(u64 stream_id) override;
    virtual void destroy_media_stream(u64 stream_id) override;
    virtual void add_media_stream_chunk(u64 stream_id, u64 offset, ByteBuffer data) override;
    virtual void set_media_stream_expected_size(u64 stream_id, u64 size) override;
    virtual void close_media_stream(u64 stream_id) override;
    virtual void set_media_stream_may_idle(u64 stream_id, bool may_idle) override;

    virtual void create_playback_session(u64 session_id, Media::AudioOutput audio_output) override;
    virtual void destroy_playback_session(u64 session_id) override;
    virtual void add_media_stream_source(u64 session_id, u64 stream_id) override;
    virtual void start_playback(u64 session_id) override;
    virtual void play(u64 session_id) override;
    virtual void pause(u64 session_id) override;
    virtual void seek(u64 session_id, u64 seek_request_id, AK::Duration timestamp, Media::SeekMode mode) override;
    virtual void set_volume(u64 session_id, double volume) override;
    virtual void set_playback_rate(u64 session_id, float rate) override;
    virtual void set_duration(u64 session_id, AK::Duration duration) override;
    virtual void set_audio_track_enabled(u64 session_id, u64 seek_request_id, Media::Track track, bool enabled, bool resume_ended_playback) override;
    virtual void reserve_video_sink(u64 session_id, u64 seek_request_id, Media::Track track, Media::VideoSinkHandle handle, bool resume_ended_playback) override;
    virtual void disable_video_sink(u64 session_id, u64 seek_request_id, Media::VideoSinkHandle handle) override;
    virtual void set_video_sink_ticking(u64 session_id, Media::VideoSinkHandle handle, bool ticking) override;
    virtual Messages::MediaServer::MapPresentedFrameSlotResponse map_presented_frame_slot(u64 session_id, Media::VideoSinkHandle handle, Media::VideoFramePoolID pool_id, u32 slot_index) override;

    virtual void create_source_buffer(u64 session_id, u64 source_buffer_id) override;
    virtual void destroy_source_buffer(u64 session_id, u64 source_buffer_id) override;
    virtual void set_source_buffer_content_type(u64 session_id, u64 source_buffer_id, String subtype) override;
    virtual void add_to_source_buffer_input(u64 session_id, u64 source_buffer_id, ByteBuffer data) override;
    virtual void run_source_buffer_append(u64 session_id, u64 source_buffer_id, u64 append_generation) override;
    virtual void reset_source_buffer_parser(u64 session_id, u64 source_buffer_id) override;
    virtual void remove_source_buffer_coded_frames(u64 session_id, u64 source_buffer_id, AK::Duration start, AK::Duration end) override;
    virtual void run_source_buffer_coded_frame_eviction(u64 session_id, u64 source_buffer_id, u64 new_data_size) override;
    virtual Messages::MediaServer::RunSourceBufferCodedFrameEvictionSynchronouslyResponse run_source_buffer_coded_frame_eviction_synchronously(u64 session_id, u64 source_buffer_id, u64 new_data_size) override;
    virtual void set_source_buffer_mode(u64 session_id, u64 source_buffer_id, Media::MediaSourceExtensions::AppendMode mode) override;
    virtual void set_source_buffer_timestamp_offset(u64 session_id, u64 source_buffer_id, AK::Duration timestamp_offset) override;
    virtual void set_source_buffer_append_window(u64 session_id, u64 source_buffer_id, AK::Duration start, AK::Duration end) override;
    virtual void set_source_buffer_generate_timestamps_flag(u64 session_id, u64 source_buffer_id, bool flag) override;
    virtual void set_source_buffer_pending_initialization_segment_for_change_type_flag(u64 session_id, u64 source_buffer_id, bool flag) override;
    virtual void set_source_buffer_reached_end_of_stream(u64 session_id, u64 source_buffer_id, bool reached) override;

    static ErrorOr<IPC::TransportHandle> create_renderer_connection();
    bool verify_renderer_role();
    PlaybackSession* find_playback_session(u64 session_id);
    Media::IncrementallyPopulatedStream* find_media_stream(u64 stream_id);

    Role m_role { Role::Renderer };

    HashMap<u64, NonnullOwnPtr<PlaybackSession>> m_playback_sessions;
    HashMap<u64, NonnullRefPtr<Media::IncrementallyPopulatedStream>> m_media_streams;
    RefPtr<Media::VideoPresentationServerConnection> m_video_presentation_connection;
};

}
