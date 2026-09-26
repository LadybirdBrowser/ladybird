/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibMediaClient/Client.h>
#include <LibMediaClient/RemoteMediaStream.h>
#include <LibMediaClient/RemotePlaybackManager.h>

namespace MediaClient {

static constexpr int IDLE_EXIT_DELAY_MS = 30'000;

static NeverDestroyed<Client::TransportFactory> s_transport_factory;
static NeverDestroyed<RefPtr<Client>> s_client;
static u64 s_next_generation { 1 };

void Client::set_transport_factory(TransportFactory factory)
{
    *s_transport_factory = move(factory);
}

ErrorOr<NonnullRefPtr<Client>> Client::acquire()
{
    if (*s_client)
        return NonnullRefPtr(**s_client);
    if (!*s_transport_factory)
        return Error::from_string_literal("No media server transport factory is installed");

    auto transport = TRY((*s_transport_factory)());
    auto client = TRY(try_make_ref_counted<Client>(move(transport)));
#ifdef AK_OS_WINDOWS
    auto response = client->send_sync<InitTransport>(Core::System::getpid());
    client->transport().set_peer_pid(response->peer_pid());
#endif
    *s_client = client;
    client->update_idle_timer();
    return client;
}

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<MediaClientEndpoint, MediaServerEndpoint>(*this, move(transport))
    , m_generation(s_next_generation++)
{
}

Client::~Client() = default;

void Client::die()
{
    verify_event_loop();
    if (*s_client == this)
        *s_client = nullptr;

    auto media_streams = move(m_media_streams);
    auto playback_managers = move(m_playback_managers);
    for (auto& [id, playback_manager] : playback_managers)
        playback_manager->connection_lost({});
    auto pending_audio_data_decodes = move(m_pending_audio_data_decodes);
    for (auto& [id, callback] : pending_audio_data_decodes)
        callback(Media::DecoderError::with_description(Media::DecoderErrorCategory::Unknown, "The media server is gone"sv));
    auto pending_file_media_support_requests = move(m_pending_file_media_support_requests);
    for (auto& [id, promise] : pending_file_media_support_requests)
        promise->reject(Error::from_string_literal("The media server is gone"));
    auto pending_decoder_capabilities_requests = move(m_pending_decoder_capabilities_requests);
    for (auto& [id, promise] : pending_decoder_capabilities_requests)
        promise->reject(Error::from_string_literal("The media server is gone"));
    if (on_death)
        on_death();
}

u64 Client::allocate_id()
{
    return m_next_id++;
}

ErrorOr<IPC::TransportHandle> Client::create_video_presentation_channel()
{
    auto response = send_sync_but_allow_failure<Messages::MediaServer::CreateVideoPresentationChannel>();
    if (!response || !response->handle().has_value())
        return Error::from_string_literal("The media server did not create a video presentation channel");
    return response->take_handle().release_value();
}

Media::MediaSupportInfo Client::query_file_media_support(StringView type, StringView subtype, Optional<String> codecs_parameter)
{
    auto response = send_sync_but_allow_failure<Messages::MediaServer::QueryFileMediaSupport>(String::from_utf8_without_validation(type.bytes()), String::from_utf8_without_validation(subtype.bytes()), move(codecs_parameter));
    if (!response)
        return {};
    return response->info();
}

Optional<Media::DecoderCapabilities> Client::query_decoder_capabilities(StringView codecs_parameter)
{
    auto response = send_sync_but_allow_failure<Messages::MediaServer::QueryDecoderCapabilities>(String::from_utf8_without_validation(codecs_parameter.bytes()));
    if (!response)
        return {};
    return response->capabilities();
}

NonnullRefPtr<Client::FileMediaSupportPromise> Client::request_file_media_support(String const& type, String const& subtype, Optional<String const&> codecs_parameter)
{
    if (!is_open())
        return FileMediaSupportPromise::rejected(Error::from_string_literal("The media server is gone"));
    auto promise = FileMediaSupportPromise::construct();
    auto request_id = allocate_id();
    m_pending_file_media_support_requests.set(request_id, promise);
    update_idle_timer();
    async_request_file_media_support(request_id, type, subtype, codecs_parameter.copy());
    return promise;
}

void Client::file_media_support_reported(u64 request_id, Media::MediaSupportInfo info)
{
    auto promise = m_pending_file_media_support_requests.take(request_id);
    if (!promise.has_value())
        return;
    update_idle_timer();
    (*promise)->resolve(info);
}

NonnullRefPtr<Client::DecoderCapabilitiesPromise> Client::request_decoder_capabilities(StringView codecs_parameter)
{
    if (!is_open())
        return DecoderCapabilitiesPromise::rejected(Error::from_string_literal("The media server is gone"));
    auto promise = DecoderCapabilitiesPromise::construct();
    auto request_id = allocate_id();
    m_pending_decoder_capabilities_requests.set(request_id, promise);
    update_idle_timer();
    async_request_decoder_capabilities(request_id, String::from_utf8_without_validation(codecs_parameter.bytes()));
    return promise;
}

void Client::decoder_capabilities_reported(u64 request_id, Optional<Media::DecoderCapabilities> capabilities)
{
    auto promise = m_pending_decoder_capabilities_requests.take(request_id);
    if (!promise.has_value())
        return;
    update_idle_timer();
    (*promise)->resolve(capabilities);
}

void Client::decode_audio_data(ReadonlyBytes data, u32 output_sample_rate, DecodeAudioDataCallback callback)
{
    auto shared_data_or_error = Core::AnonymousBuffer::create_with_size(data.size());
    if (shared_data_or_error.is_error() || !is_open()) {
        callback(Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Unable to hand the audio data to the media server"sv));
        return;
    }
    auto shared_data = shared_data_or_error.release_value();
    data.copy_to({ shared_data.data<u8>(), shared_data.size() });

    auto request_id = allocate_id();
    m_pending_audio_data_decodes.set(request_id, move(callback));
    update_idle_timer();
    async_decode_audio_data(request_id, move(shared_data), output_sample_rate);
}

void Client::audio_data_decoded(u64 request_id, u32 sample_rate, u32 channel_count, u64 frame_count, Core::AnonymousBuffer planar_samples)
{
    auto callback = m_pending_audio_data_decodes.take(request_id);
    if (!callback.has_value())
        return;
    update_idle_timer();
    Checked<u64> byte_count = frame_count;
    byte_count *= channel_count;
    byte_count *= sizeof(float);
    if (channel_count == 0 || frame_count == 0 || byte_count.has_overflow() || !planar_samples.is_valid() || planar_samples.size() < byte_count.value()) {
        (*callback)(Media::DecoderError::with_description(Media::DecoderErrorCategory::Corrupted, "The media server returned malformed audio"sv));
        return;
    }
    (*callback)(DecodedAudioData { sample_rate, channel_count, frame_count, move(planar_samples) });
}

void Client::audio_data_decode_failed(u64 request_id, Media::DecoderError error)
{
    auto callback = m_pending_audio_data_decodes.take(request_id);
    if (!callback.has_value())
        return;
    update_idle_timer();
    (*callback)(move(error));
}

void Client::register_media_stream(Badge<RemoteMediaStream>, RemoteMediaStream& stream)
{
    m_media_streams.set(stream.id(), &stream);
    update_idle_timer();
}

void Client::unregister_media_stream(Badge<RemoteMediaStream>, RemoteMediaStream& stream)
{
    m_media_streams.remove(stream.id());
    update_idle_timer();
}

void Client::register_playback_manager(Badge<RemotePlaybackManager>, RemotePlaybackManager& playback_manager)
{
    m_playback_managers.set(playback_manager.session_id(), &playback_manager);
    update_idle_timer();
}

void Client::unregister_playback_manager(Badge<RemotePlaybackManager>, RemotePlaybackManager& playback_manager)
{
    m_playback_managers.remove(playback_manager.session_id());
    update_idle_timer();
}

void Client::update_idle_timer()
{
    auto is_idle = [&] {
        if (!m_media_streams.is_empty() || !m_playback_managers.is_empty())
            return false;
        if (!m_pending_audio_data_decodes.is_empty())
            return false;
        return m_pending_file_media_support_requests.is_empty() && m_pending_decoder_capabilities_requests.is_empty();
    }();
    if (!is_idle) {
        if (m_idle_timer)
            m_idle_timer->stop();
        return;
    }
    if (!m_idle_timer) {
        m_idle_timer = Core::Timer::create_single_shot(IDLE_EXIT_DELAY_MS, [this] {
            // Clearing the client reference closes the connection and causes the server to exit.
            if (*s_client == this)
                *s_client = nullptr;
        });
    }
    m_idle_timer->restart();
}

RemotePlaybackManager* Client::find_playback_manager(u64 session_id)
{
    return m_playback_managers.get(session_id).value_or(nullptr);
}

void Client::media_stream_data_requested(u64 stream_id, Optional<u64> offset)
{
    if (auto* stream = m_media_streams.get(stream_id).value_or(nullptr))
        stream->data_requested({}, offset);
}

void Client::playback_session_clock_changed(u64 session_id, Media::MediaTimeReader time_reader)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->clock_changed({}, move(time_reader));
}

void Client::playback_session_metadata_parsed(u64 session_id, Vector<Media::Track> audio_tracks, Vector<Media::Track> video_tracks, Optional<Media::Track> preferred_audio_track, Optional<Media::Track> preferred_video_track, Optional<UnixDateTime> start_time_realtime)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->metadata_parsed({}, audio_tracks, video_tracks, move(preferred_audio_track), move(preferred_video_track), start_time_realtime);
}

void Client::playback_session_duration_changed(u64 session_id, AK::Duration duration)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->duration_changed({}, duration);
}

void Client::playback_session_state_changed(u64 session_id, u64 applied_seek_request_id, Media::PlaybackState state, bool is_playing, Media::AvailableData available_data, AK::Duration current_time)
{
    if (state > Media::PlaybackState::Ended || available_data > Media::AvailableData::Future) {
        dbgln("MediaClient: Ignoring invalid playback state");
        return;
    }
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->state_changed({}, applied_seek_request_id, state, is_playing, available_data, current_time);
}

void Client::playback_session_buffered_ranges_changed(u64 session_id, Media::TimeRanges buffered_ranges)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->buffered_ranges_changed({}, buffered_ranges);
}

void Client::playback_session_error(u64 session_id, Media::DecoderError error)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->error({}, move(error));
}

void Client::playback_session_video_resized(u64 session_id, Media::VideoSinkHandle handle, u32 width, u32 height)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->video_resized({}, handle, Gfx::Size<u32> { width, height });
}

void Client::playback_session_video_edge_attached(u64 session_id, Media::VideoSinkHandle handle, Media::PresentedFramePage presented_frame_page)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->video_edge_attached({}, handle, move(presented_frame_page));
}

void Client::playback_session_video_frame_pool_retired(u64 session_id, Media::VideoSinkHandle handle, Media::VideoFramePoolID pool_id)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->video_frame_pool_retired({}, handle, pool_id);
}

void Client::source_buffer_duration_received(u64 session_id, u64 source_buffer_id, double duration)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->source_buffer_duration_received({}, source_buffer_id, duration);
}

void Client::source_buffer_first_initialization_segment_received(u64 session_id, u64 source_buffer_id, Vector<Media::Track> audio_tracks, Vector<Media::Track> video_tracks, Vector<Media::Track> text_tracks)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->source_buffer_first_initialization_segment_received({}, source_buffer_id, audio_tracks, video_tracks, text_tracks);
}

void Client::source_buffer_coded_frames_processed(u64 session_id, u64 source_buffer_id, AK::Duration group_end_timestamp)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->source_buffer_coded_frames_processed({}, source_buffer_id, group_end_timestamp);
}

void Client::source_buffer_append_completed(u64 session_id, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState state)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->source_buffer_append_completed({}, source_buffer_id, append_generation, move(state));
}

void Client::source_buffer_append_failed(u64 session_id, u64 source_buffer_id, u64 append_generation, Media::MediaSourceExtensions::PublishedState state)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->source_buffer_append_failed({}, source_buffer_id, append_generation, move(state));
}

void Client::source_buffer_removal_completed(u64 session_id, u64 source_buffer_id, Media::MediaSourceExtensions::PublishedState state)
{
    if (auto* playback_manager = find_playback_manager(session_id))
        playback_manager->source_buffer_removal_completed({}, source_buffer_id, move(state));
}

void Client::verify_event_loop() const
{
    if (Core::EventLoop::is_running())
        VERIFY(&Core::EventLoop::current() == m_creation_event_loop);
}

}
