/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecodeAudioStream.h>
#include <LibMedia/DecoderRegistry.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/MediaSourceExtensions/ByteStreamParser.h>
#include <LibMedia/MediaSupport.h>
#include <LibMedia/PlaybackManager.h>
#include <LibThreading/ThreadPool.h>
#include <MediaServer/ConnectionFromClient.h>
#include <MediaServer/PlaybackSession.h>

namespace MediaServer {

namespace Commands = Media::MediaSourceExtensions::Commands;

static NeverDestroyed<HashMap<int, NonnullRefPtr<ConnectionFromClient>>> s_connections;
static int s_next_client_id { 1 };

static int allocate_client_id()
{
    VERIFY(s_next_client_id != NumericLimits<int>::max());
    return s_next_client_id++;
}

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, Role role)
    : IPC::ConnectionFromClient<MediaClientEndpoint, MediaServerEndpoint>(*this, move(transport), allocate_client_id())
    , m_role(role)
{
    s_connections->set(client_id(), *this);
}

ConnectionFromClient::~ConnectionFromClient() = default;

void ConnectionFromClient::die()
{
    s_connections->remove(client_id());
    Core::Process::terminate_immediately(0);
}

Messages::MediaServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#else
    did_misbehave("Unexpected media server transport initialization");
    return 0;
#endif
}

ErrorOr<IPC::TransportHandle> ConnectionFromClient::create_renderer_connection()
{
    auto paired_transports = TRY(IPC::Transport::create_paired());
    auto handle = move(paired_transports.remote_handle);

    // The static connection map owns this connection until its peer disconnects.
    auto client = adopt_ref(*new ConnectionFromClient(move(paired_transports.local), Role::Renderer));

    return handle;
}

Messages::MediaServer::ConnectNewClientResponse ConnectionFromClient::connect_new_client()
{
    if (m_role != Role::Controller) {
        did_misbehave("Only the controller may connect a new media client");
        return OptionalNone {};
    }

    auto handle = create_renderer_connection();
    if (handle.is_error()) {
        dbgln("Failed to connect a media client: {}", handle.error());
        return OptionalNone {};
    }
    return handle.release_value();
}

bool ConnectionFromClient::verify_renderer_role()
{
    if (m_role == Role::Renderer)
        return true;
    did_misbehave("Only the renderer may use media sessions");
    return false;
}

Messages::MediaServer::CreateVideoPresentationChannelResponse ConnectionFromClient::create_video_presentation_channel()
{
    if (!verify_renderer_role())
        return OptionalNone {};

    auto paired_transports_or_error = IPC::Transport::create_paired();
    if (paired_transports_or_error.is_error()) {
        dbgln("Failed to create a video presentation channel: {}", paired_transports_or_error.error());
        return OptionalNone {};
    }
    auto paired_transports = paired_transports_or_error.release_value();

    // A replaced connection releases its edges; the presentation client that held them is gone or will ask again.
    m_video_presentation_connection = Media::VideoPresentationServerConnection::construct(move(paired_transports.local));
#ifdef AK_OS_WINDOWS
    m_video_presentation_connection->transport().set_peer_pid(transport().peer_pid());
#endif
    return move(paired_transports.remote_handle);
}

static Media::MediaSupportInfo file_media_support_for(String type, String subtype, Optional<String> codecs_parameter)
{
    OrderedHashMap<String, String> parameters;
    if (codecs_parameter.has_value())
        parameters.set("codecs"_string, codecs_parameter.release_value());
    return Media::file_media_support({ move(type), move(subtype), parameters });
}

// The type is decodable only if every codec it lists is, and it plays as well as its weakest codec does.
static Optional<Media::DecoderCapabilities> decoder_capabilities_for_codecs(StringView codecs_parameter)
{
    Optional<Media::DecoderCapabilities> combined_capabilities;
    for (auto codec_string : codecs_parameter.split_view(',', SplitBehavior::KeepEmpty)) {
        auto codec = Media::parse_codec_parameters_string(codec_string.trim_whitespace());
        if (!codec.has_value())
            return {};
        auto codec_capabilities = Media::decoder_capabilities(*codec);
        if (!codec_capabilities.has_value())
            return {};
        if (!combined_capabilities.has_value()) {
            combined_capabilities = codec_capabilities;
            continue;
        }
        combined_capabilities->smooth &= codec_capabilities->smooth;
        combined_capabilities->power_efficient &= codec_capabilities->power_efficient;
    }
    return combined_capabilities;
}

Messages::MediaServer::QueryFileMediaSupportResponse ConnectionFromClient::query_file_media_support(String type, String subtype, Optional<String> codecs_parameter)
{
    return file_media_support_for(move(type), move(subtype), move(codecs_parameter));
}

Messages::MediaServer::QueryDecoderCapabilitiesResponse ConnectionFromClient::query_decoder_capabilities(String codecs_parameter)
{
    return decoder_capabilities_for_codecs(codecs_parameter);
}

void ConnectionFromClient::request_file_media_support(u64 request_id, String type, String subtype, Optional<String> codecs_parameter)
{
    async_file_media_support_reported(request_id, file_media_support_for(move(type), move(subtype), move(codecs_parameter)));
}

void ConnectionFromClient::request_decoder_capabilities(u64 request_id, String codecs_parameter)
{
    async_decoder_capabilities_reported(request_id, decoder_capabilities_for_codecs(codecs_parameter));
}

struct DecodedAudioSamples {
    u32 sample_rate { 0 };
    u32 channel_count { 0 };
    u64 frame_count { 0 };
    Core::AnonymousBuffer planar_samples;
};

static Media::DecoderErrorOr<DecodedAudioSamples> decode_audio_data_to_shared_buffer(Core::AnonymousBuffer const& data, u32 output_sample_rate)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_data({ data.data<u8>(), data.size() });
    auto decoded = TRY(Media::decode_entire_audio_stream(stream, output_sample_rate));
    if (decoded.channels.is_empty() || decoded.channels.first().is_empty())
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::Corrupted, "The stream decoded to no audio"sv);

    auto frame_count = decoded.channels.first().size();
    Checked<size_t> byte_count = frame_count;
    byte_count *= decoded.channels.size();
    byte_count *= sizeof(float);
    if (byte_count.has_overflow())
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Decoded audio is too large to share"sv);
    auto planar_samples_or_error = Core::AnonymousBuffer::create_with_size(byte_count.value());
    if (planar_samples_or_error.is_error())
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Unable to allocate shared sample storage"sv);
    auto planar_samples = planar_samples_or_error.release_value();

    auto* destination = planar_samples.data<float>();
    for (auto const& channel : decoded.channels) {
        VERIFY(channel.size() == frame_count);
        memcpy(destination, channel.data(), frame_count * sizeof(float));
        destination += frame_count;
    }
    return DecodedAudioSamples { decoded.sample_specification.sample_rate(), static_cast<u32>(decoded.channels.size()), frame_count, move(planar_samples) };
}

void ConnectionFromClient::decode_audio_data(u64 request_id, Core::AnonymousBuffer data, u32 output_sample_rate)
{
    if (!verify_renderer_role())
        return;
    if (!data.is_valid() || output_sample_rate == 0) {
        did_misbehave("Invalid audio data decode request");
        return;
    }

    auto& main_thread_event_loop = Core::EventLoop::current();
    Threading::ThreadPool::the().submit([strong_this = NonnullRefPtr(*this), &main_thread_event_loop, request_id, data = move(data), output_sample_rate] mutable {
        auto result = decode_audio_data_to_shared_buffer(data, output_sample_rate);
        main_thread_event_loop.deferred_invoke([strong_this = move(strong_this), request_id, result = move(result)] mutable {
            if (!strong_this->is_open())
                return;
            if (result.is_error()) {
                strong_this->async_audio_data_decode_failed(request_id, result.release_error());
                return;
            }
            auto decoded = result.release_value();
            strong_this->async_audio_data_decoded(request_id, decoded.sample_rate, decoded.channel_count, decoded.frame_count, move(decoded.planar_samples));
        });
    });
}

Media::IncrementallyPopulatedStream* ConnectionFromClient::find_media_stream(u64 stream_id)
{
    auto it = m_media_streams.find(stream_id);
    if (it == m_media_streams.end())
        return nullptr;
    return it->value.ptr();
}

void ConnectionFromClient::create_media_stream(u64 stream_id)
{
    if (!verify_renderer_role())
        return;
    if (m_media_streams.contains(stream_id)) {
        did_misbehave("Duplicate media stream ID");
        return;
    }
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_data_request_callback([this, stream_id](Optional<u64> offset) {
        async_media_stream_data_requested(stream_id, offset);
    });
    m_media_streams.set(stream_id, move(stream));
}

void ConnectionFromClient::destroy_media_stream(u64 stream_id)
{
    auto stream = m_media_streams.take(stream_id);
    if (!stream.has_value())
        return;
    (*stream)->set_data_request_callback(nullptr);
    (*stream)->close();
}

void ConnectionFromClient::add_media_stream_chunk(u64 stream_id, u64 offset, ByteBuffer data)
{
    if (auto* stream = find_media_stream(stream_id))
        stream->add_chunk_at(offset, data.bytes());
}

void ConnectionFromClient::set_media_stream_expected_size(u64 stream_id, u64 size)
{
    if (auto* stream = find_media_stream(stream_id))
        stream->set_expected_size(size);
}

void ConnectionFromClient::close_media_stream(u64 stream_id)
{
    if (auto* stream = find_media_stream(stream_id))
        stream->close();
}

void ConnectionFromClient::set_media_stream_may_idle(u64 stream_id, bool may_idle)
{
    if (auto* stream = find_media_stream(stream_id))
        stream->set_may_idle(may_idle);
}

PlaybackSession* ConnectionFromClient::find_playback_session(u64 session_id)
{
    auto it = m_playback_sessions.find(session_id);
    if (it == m_playback_sessions.end())
        return nullptr;
    return it->value.ptr();
}

void ConnectionFromClient::create_playback_session(u64 session_id, Media::AudioOutput audio_output)
{
    if (!verify_renderer_role())
        return;
    if (m_playback_sessions.contains(session_id)) {
        did_misbehave("Duplicate playback session ID");
        return;
    }
    m_playback_sessions.set(session_id, make<PlaybackSession>(*this, session_id, audio_output));
}

void ConnectionFromClient::destroy_playback_session(u64 session_id)
{
    m_playback_sessions.remove(session_id);
}

void ConnectionFromClient::add_media_stream_source(u64 session_id, u64 stream_id)
{
    auto* session = find_playback_session(session_id);
    auto* stream = find_media_stream(stream_id);
    if (!session || !stream)
        return;
    session->add_media_stream_source(stream_id, NonnullRefPtr<Media::MediaStream>(*stream));
}

void ConnectionFromClient::start_playback(u64 session_id)
{
    if (auto* session = find_playback_session(session_id))
        session->manager().start();
}

void ConnectionFromClient::play(u64 session_id)
{
    if (auto* session = find_playback_session(session_id))
        session->manager().play();
}

void ConnectionFromClient::pause(u64 session_id)
{
    if (auto* session = find_playback_session(session_id))
        session->manager().pause();
}

void ConnectionFromClient::seek(u64 session_id, u64 seek_request_id, AK::Duration timestamp, Media::SeekMode mode)
{
    if (mode > Media::SeekMode::FastAfter) {
        did_misbehave("Invalid seek mode");
        return;
    }
    if (auto* session = find_playback_session(session_id))
        session->seek(seek_request_id, timestamp, mode);
}

void ConnectionFromClient::set_volume(u64 session_id, double volume)
{
    if (auto* session = find_playback_session(session_id))
        session->manager().set_volume(volume);
}

void ConnectionFromClient::set_playback_rate(u64 session_id, float rate)
{
    if (!isfinite(rate) || rate < 0) {
        did_misbehave("Invalid playback rate");
        return;
    }
    if (auto* session = find_playback_session(session_id))
        session->manager().set_playback_rate(rate);
}

void ConnectionFromClient::set_duration(u64 session_id, AK::Duration duration)
{
    if (auto* session = find_playback_session(session_id))
        session->manager().set_duration(duration);
}

void ConnectionFromClient::set_audio_track_enabled(u64 session_id, u64 seek_request_id, Media::Track track, bool enabled, bool resume_ended_playback)
{
    auto* session = find_playback_session(session_id);
    if (!session)
        return;
    session->set_audio_track_enabled(seek_request_id, track, enabled, resume_ended_playback);
}

void ConnectionFromClient::reserve_video_sink(u64 session_id, u64 seek_request_id, Media::Track track, Media::VideoSinkHandle handle, bool resume_ended_playback)
{
    auto* session = find_playback_session(session_id);
    if (!session)
        return;
    session->reserve_video_sink(seek_request_id, track, handle, resume_ended_playback);
    if (m_video_presentation_connection)
        m_video_presentation_connection->retry_pending_video_edges();
}

void ConnectionFromClient::disable_video_sink(u64 session_id, u64 seek_request_id, Media::VideoSinkHandle handle)
{
    if (auto* session = find_playback_session(session_id))
        session->disable_video_sink(seek_request_id, handle);
}

void ConnectionFromClient::set_video_sink_ticking(u64 session_id, Media::VideoSinkHandle handle, bool ticking)
{
    if (find_playback_session(session_id))
        Media::PlaybackManager::set_video_sink_ticking(handle, ticking);
}

Messages::MediaServer::MapPresentedFrameSlotResponse ConnectionFromClient::map_presented_frame_slot(u64 session_id, Media::VideoSinkHandle handle, Media::VideoFramePoolID pool_id, u32 slot_index)
{
    if (!find_playback_session(session_id))
        return { OptionalNone {}, nullptr };
    auto storage = Media::PlaybackManager::presented_frame_slot_storage(handle, pool_id, slot_index);
    if (!storage.has_value())
        return { OptionalNone {}, nullptr };
    return { move(storage->buffer), move(storage->surface) };
}

void ConnectionFromClient::create_source_buffer(u64 session_id, u64 source_buffer_id)
{
    if (auto* session = find_playback_session(session_id))
        session->create_source_buffer(source_buffer_id);
}

void ConnectionFromClient::destroy_source_buffer(u64 session_id, u64 source_buffer_id)
{
    if (auto* session = find_playback_session(session_id))
        session->destroy_source_buffer(source_buffer_id);
}

void ConnectionFromClient::set_source_buffer_content_type(u64 session_id, u64 source_buffer_id, String subtype)
{
    if (auto* session = find_playback_session(session_id))
        session->set_source_buffer_content_type(source_buffer_id, subtype);
}

void ConnectionFromClient::add_to_source_buffer_input(u64 session_id, u64 source_buffer_id, ByteBuffer data)
{
    if (auto* session = find_playback_session(session_id))
        session->add_to_source_buffer_input(source_buffer_id, data.bytes());
}

void ConnectionFromClient::run_source_buffer_append(u64 session_id, u64 source_buffer_id, u64 append_generation)
{
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_append(source_buffer_id, append_generation);
}

void ConnectionFromClient::reset_source_buffer_parser(u64 session_id, u64 source_buffer_id)
{
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_command(source_buffer_id, Commands::ResetParserState {});
}

void ConnectionFromClient::remove_source_buffer_coded_frames(u64 session_id, u64 source_buffer_id, AK::Duration start, AK::Duration end)
{
    if (auto* session = find_playback_session(session_id))
        session->remove_source_buffer_coded_frames(source_buffer_id, start, end);
}

void ConnectionFromClient::set_source_buffer_mode(u64 session_id, u64 source_buffer_id, Media::MediaSourceExtensions::AppendMode mode)
{
    if (mode > Media::MediaSourceExtensions::AppendMode::Sequence) {
        did_misbehave("Invalid append mode");
        return;
    }
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_command(source_buffer_id, Commands::SetMode { mode });
}

void ConnectionFromClient::set_source_buffer_timestamp_offset(u64 session_id, u64 source_buffer_id, AK::Duration timestamp_offset)
{
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_command(source_buffer_id, Commands::SetTimestampOffset { timestamp_offset });
}

void ConnectionFromClient::set_source_buffer_append_window(u64 session_id, u64 source_buffer_id, AK::Duration start, AK::Duration end)
{
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_command(source_buffer_id, Commands::SetAppendWindow { start, end });
}

void ConnectionFromClient::set_source_buffer_generate_timestamps_flag(u64 session_id, u64 source_buffer_id, bool flag)
{
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_command(source_buffer_id, Commands::SetGenerateTimestampsFlag { flag });
}

void ConnectionFromClient::set_source_buffer_pending_initialization_segment_for_change_type_flag(u64 session_id, u64 source_buffer_id, bool flag)
{
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_command(source_buffer_id, Commands::SetPendingInitializationSegmentForChangeTypeFlag { flag });
}

void ConnectionFromClient::set_source_buffer_reached_end_of_stream(u64 session_id, u64 source_buffer_id, bool reached)
{
    if (auto* session = find_playback_session(session_id))
        session->run_source_buffer_command(source_buffer_id, Commands::SetReachedEndOfStream { reached });
}

}
