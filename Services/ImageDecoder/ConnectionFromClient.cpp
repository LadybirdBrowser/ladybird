/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/IDAllocator.h>
#include <ImageDecoder/ConnectionFromClient.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>

namespace ImageDecoder {

static HashMap<int, RefPtr<ConnectionFromClient>> s_connections;
static IDAllocator s_client_ids;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint>(*this, move(transport), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);
}

void ConnectionFromClient::die()
{
    for (auto& [_, job] : m_pending_jobs)
        job->cancel();
    m_pending_jobs.clear();

    for (auto& [_, job] : m_pending_frame_jobs)
        job->cancel();
    m_pending_frame_jobs.clear();
    m_animation_sessions.clear();

    auto client_id = this->client_id();
    s_connections.remove(client_id);
    s_client_ids.deallocate(client_id);

    if (s_connections.is_empty()) {
        Threading::quit_background_thread();
        Core::EventLoop::current().quit(0);
    }
}

Messages::ImageDecoderServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

ErrorOr<IPC::File> ConnectionFromClient::connect_new_client()
{
    int socket_fds[2] {};
    if (auto err = Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds); err.is_error())
        return err.release_error();

    auto client_socket_or_error = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (client_socket_or_error.is_error()) {
        (void)Core::System::close(socket_fds[0]);
        (void)Core::System::close(socket_fds[1]);
        return client_socket_or_error.release_error();
    }

    auto client_socket = client_socket_or_error.release_value();
    // Note: A ref is stored in the static s_connections map
    auto client = adopt_ref(*new ConnectionFromClient(make<IPC::Transport>(move(client_socket))));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::ImageDecoderServer::ConnectNewClientsResponse ConnectionFromClient::connect_new_clients(size_t count)
{
    Vector<IPC::File> files;
    files.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i) {
        auto file_or_error = connect_new_client();
        if (file_or_error.is_error()) {
            dbgln("Failed to connect new client: {}", file_or_error.error());
            return Vector<IPC::File> {};
        }
        files.unchecked_append(file_or_error.release_value());
    }
    return files;
}

static void decode_image_to_bitmaps_and_durations_with_decoder(Gfx::ImageDecoder const& decoder, Optional<Gfx::IntSize> ideal_size, Vector<RefPtr<Gfx::Bitmap>>& bitmaps, Vector<u32>& durations)
{
    bitmaps.ensure_capacity(decoder.frame_count());
    durations.ensure_capacity(decoder.frame_count());
    for (size_t i = 0; i < decoder.frame_count(); ++i) {
        auto frame_or_error = decoder.frame(i, ideal_size);
        if (frame_or_error.is_error()) {
            bitmaps.unchecked_append({});
            durations.unchecked_append(0);
        } else {
            auto frame = frame_or_error.release_value();
            frame.image->set_alpha_type_destructive(Gfx::AlphaType::Premultiplied);
            bitmaps.unchecked_append(frame.image);
            durations.unchecked_append(frame.duration);
        }
    }
}

static constexpr u32 STREAMING_BATCH_SIZE = 4;

static ErrorOr<ConnectionFromClient::DecodeResult> decode_image_to_details(Core::AnonymousBuffer encoded_buffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> const& known_mime_type)
{
    auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(ReadonlyBytes { encoded_buffer.data<u8>(), encoded_buffer.size() }, known_mime_type));

    if (!decoder)
        return Error::from_string_literal("Could not find suitable image decoder plugin for data");

    if (!decoder->frame_count())
        return Error::from_string_literal("Could not decode image from encoded data");

    ConnectionFromClient::DecodeResult result;
    result.is_animated = decoder->is_animated();
    result.loop_count = decoder->loop_count();
    result.frame_count = decoder->frame_count();

    if (auto maybe_icc_data = decoder->color_space(); !maybe_icc_data.is_error())
        result.color_profile = maybe_icc_data.value();
    else
        dbgln("Invalid color profile: {}", maybe_icc_data.error());

    Vector<RefPtr<Gfx::Bitmap>> bitmaps;

    if (auto maybe_metadata = decoder->metadata(); maybe_metadata.has_value() && is<Gfx::ExifMetadata>(*maybe_metadata)) {
        auto const& exif = static_cast<Gfx::ExifMetadata const&>(maybe_metadata.value());
        if (exif.x_resolution().has_value() && exif.y_resolution().has_value()) {
            auto const x_resolution = exif.x_resolution()->as_double();
            auto const y_resolution = exif.y_resolution()->as_double();
            if (x_resolution < y_resolution)
                result.scale.set_y(x_resolution / y_resolution);
            else
                result.scale.set_x(y_resolution / x_resolution);
        }
    }

    bool const use_streaming = result.is_animated && result.frame_count > 1;

    if (use_streaming) {
        // Collect all durations cheaply (via frame_duration(), no pixel decode for GIF).
        result.durations.ensure_capacity(result.frame_count);
        for (u32 i = 0; i < result.frame_count; ++i)
            result.durations.unchecked_append(decoder->frame_duration(i));

        // Decode only the first batch of frames.
        u32 const batch_size = min(STREAMING_BATCH_SIZE, result.frame_count);
        bitmaps.ensure_capacity(batch_size);
        for (u32 i = 0; i < batch_size; ++i) {
            auto frame_or_error = decoder->frame(i, ideal_size);
            if (frame_or_error.is_error()) {
                bitmaps.unchecked_append({});
            } else {
                auto frame = frame_or_error.release_value();
                frame.image->set_alpha_type_destructive(Gfx::AlphaType::Premultiplied);
                bitmaps.unchecked_append(frame.image);
                // If frame_duration() returned 0, use the actual decoded duration.
                if (result.durations[i] == 0)
                    result.durations[i] = frame.duration;
            }
        }

        // Keep decoder alive for future frame requests.
        result.decoder = decoder;
        result.encoded_data = move(encoded_buffer);
    } else {
        decode_image_to_bitmaps_and_durations_with_decoder(*decoder, move(ideal_size), bitmaps, result.durations);
    }

    if (bitmaps.is_empty())
        return Error::from_string_literal("Could not decode image");

    result.bitmaps = Gfx::BitmapSequence { move(bitmaps) };

    return result;
}

NonnullRefPtr<ConnectionFromClient::Job> ConnectionFromClient::make_decode_image_job(i64 request_id, Core::AnonymousBuffer encoded_buffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type)
{
    return Job::construct(
        [encoded_buffer = move(encoded_buffer), ideal_size = move(ideal_size), mime_type = move(mime_type)](auto&) mutable -> ErrorOr<DecodeResult> {
            return TRY(decode_image_to_details(move(encoded_buffer), ideal_size, mime_type));
        },
        [strong_this = NonnullRefPtr(*this), request_id](DecodeResult result) -> ErrorOr<void> {
            i64 session_id = 0;

            if (result.decoder) {
                // This is a streaming animated decode. Create a session.
                session_id = strong_this->m_next_session_id++;
                auto session = make<AnimationSession>();
                session->encoded_data = move(result.encoded_data);
                session->decoder = move(result.decoder);
                session->frame_count = result.frame_count;
                strong_this->m_animation_sessions.set(session_id, move(session));
            }

            strong_this->async_did_decode_image(request_id, result.is_animated, result.loop_count, move(result.bitmaps), move(result.durations), result.scale, move(result.color_profile), session_id);
            strong_this->m_pending_jobs.remove(request_id);
            return {};
        },
        [strong_this = NonnullRefPtr(*this), request_id](Error error) -> void {
            if (strong_this->is_open())
                strong_this->async_did_fail_to_decode_image(request_id, MUST(String::formatted("Decoding failed: {}", error)));
            strong_this->m_pending_jobs.remove(request_id);
        });
}

void ConnectionFromClient::decode_image(Core::AnonymousBuffer encoded_buffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type, i64 request_id)
{
    if (!encoded_buffer.is_valid()) {
        dbgln_if(IMAGE_DECODER_DEBUG, "Encoded data is invalid");
        async_did_fail_to_decode_image(request_id, "Encoded data is invalid"_string);
        return;
    }

    auto set_result = m_pending_jobs.set(request_id, make_decode_image_job(request_id, move(encoded_buffer), ideal_size, move(mime_type)), AK::HashSetExistingEntryBehavior::Keep);

    if (set_result != HashSetResult::InsertedNewEntry) {
        m_pending_jobs.take(request_id).value()->cancel();
        did_misbehave("Duplicate decode request id");
        return;
    }
}

void ConnectionFromClient::cancel_decoding(i64 request_id)
{
    if (auto job = m_pending_jobs.take(request_id); job.has_value()) {
        job.value()->cancel();
    }
}

void ConnectionFromClient::request_animation_frames(i64 session_id, u32 start_frame_index, u32 count)
{
    auto it = m_animation_sessions.find(session_id);
    if (it == m_animation_sessions.end())
        return;

    auto& session = *it->value;
    auto decoder = session.decoder;
    u32 const frame_count = session.frame_count;

    if (start_frame_index >= frame_count)
        return;

    u32 const end_index = min(frame_count, start_frame_index + min(count, frame_count - start_frame_index));

    auto job = FrameDecodeJob::construct(
        [decoder, start_frame_index, end_index](auto&) -> ErrorOr<Vector<Gfx::ImageFrameDescriptor>> {
            Vector<Gfx::ImageFrameDescriptor> frames;
            frames.ensure_capacity(end_index - start_frame_index);
            for (u32 i = start_frame_index; i < end_index; ++i) {
                auto frame = TRY(decoder->frame(i));
                frame.image->set_alpha_type_destructive(Gfx::AlphaType::Premultiplied);
                frames.unchecked_append(move(frame));
            }
            return frames;
        },
        [strong_this = NonnullRefPtr(*this), session_id](Vector<Gfx::ImageFrameDescriptor> frames) -> ErrorOr<void> {
            Vector<RefPtr<Gfx::Bitmap>> bitmaps;
            bitmaps.ensure_capacity(frames.size());
            for (auto& frame : frames)
                bitmaps.unchecked_append(move(frame.image));
            strong_this->async_did_decode_animation_frames(session_id, Gfx::BitmapSequence { move(bitmaps) });
            strong_this->m_pending_frame_jobs.remove(session_id);
            return {};
        },
        [strong_this = NonnullRefPtr(*this), session_id](Error error) -> void {
            if (strong_this->is_open())
                strong_this->async_did_fail_animation_decode(session_id, MUST(String::formatted("Frame decode failed: {}", error)));
            strong_this->m_pending_frame_jobs.remove(session_id);
        });

    m_pending_frame_jobs.set(session_id, move(job));
}

void ConnectionFromClient::stop_animation_decode(i64 session_id)
{
    if (auto job = m_pending_frame_jobs.take(session_id); job.has_value())
        job.value()->cancel();
    m_animation_sessions.remove(session_id);
}

}
