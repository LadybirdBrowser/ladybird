/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/IDAllocator.h>
#include <AK/RefPtr.h>
#include <ImageDecoder/ConnectionFromClient.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>
#include <cmath>

namespace ImageDecoder {

// SECURITY LIMITS
constexpr size_t MAX_ENCODED_IMAGE_SIZE = 50 * MiB;
constexpr size_t MAX_FRAME_COUNT = 1024;
constexpr int MAX_IMAGE_DIMENSION = 16384;
constexpr size_t MAX_PENDING_JOBS = 16;
constexpr int DECODE_TIMEOUT_MS = 3000;

static HashMap<int, RefPtr<ConnectionFromClient>> s_connections;
static IDAllocator s_client_ids;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint>(*this, move(transport), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);
}

void ConnectionFromClient::die()
{
    // SECURITY: Cancel all pending decode jobs on client disconnect
    for (auto& [image_id, job] : m_pending_jobs) {
        job->cancel();
        if (is_open())
            async_did_fail_to_decode_image(image_id, "Client disconnected"_string);
    }
    m_pending_jobs.clear();

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

static void decode_image_to_bitmaps_and_durations_with_decoder(
    Gfx::ImageDecoder const& decoder,
    Optional<Gfx::IntSize> ideal_size,
    Vector<RefPtr<Gfx::Bitmap>>& bitmaps,
    Vector<u32>& durations)
{
    bitmaps.ensure_capacity(decoder.frame_count());
    durations.ensure_capacity(decoder.frame_count());

    for (size_t i = 0; i < decoder.frame_count(); ++i) {
        auto frame_or_error = decoder.frame(i, ideal_size);
        if (frame_or_error.is_error()) {
            bitmaps.unchecked_append({});
            durations.unchecked_append(0);
            continue;
        }

        auto frame = frame_or_error.release_value();

        // SECURITY: Validate decoded frame dimensions
        if (frame.image->width() > MAX_IMAGE_DIMENSION
            || frame.image->height() > MAX_IMAGE_DIMENSION) {
            bitmaps.unchecked_append({});
            durations.unchecked_append(0);
            continue;
        }

        frame.image->set_alpha_type_destructive(Gfx::AlphaType::Premultiplied);
        bitmaps.unchecked_append(frame.image);
        durations.unchecked_append(frame.duration);
    }
}

static ErrorOr<ConnectionFromClient::DecodeResult> decode_image_to_details(
    Core::AnonymousBuffer const& encoded_buffer,
    Optional<Gfx::IntSize> ideal_size,
    Optional<ByteString> const& known_mime_type)
{
    auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(
        ReadonlyBytes { encoded_buffer.data<u8>(), encoded_buffer.size() },
        known_mime_type));

    if (!decoder)
        return Error::from_string_literal("No suitable image decoder found");

    // SECURITY: Frame count limit
    if (decoder->frame_count() > MAX_FRAME_COUNT)
        return Error::from_string_literal("Too many frames in image");

    if (!decoder->frame_count())
        return Error::from_string_literal("Image contains no frames");

    ConnectionFromClient::DecodeResult result;
    result.is_animated = decoder->is_animated();
    result.loop_count = decoder->loop_count();

    if (auto maybe_icc_data = decoder->color_space(); !maybe_icc_data.is_error())
        result.color_profile = maybe_icc_data.value();

    if (auto maybe_metadata = decoder->metadata();
        maybe_metadata.has_value() && is<Gfx::ExifMetadata>(*maybe_metadata)) {

        auto const& exif = static_cast<Gfx::ExifMetadata const&>(maybe_metadata.value());

        if (exif.x_resolution().has_value() && exif.y_resolution().has_value()) {
            auto x = exif.x_resolution()->as_double();
            auto y = exif.y_resolution()->as_double();

            // SECURITY: Guard against NaN / infinity
            if (std::isfinite(x) && std::isfinite(y) && x > 0 && y > 0) {
                if (x < y)
                    result.scale.set_y(x / y);
                else
                    result.scale.set_x(y / x);
            }
        }
    }

    Vector<RefPtr<Gfx::Bitmap>> bitmaps;
    decode_image_to_bitmaps_and_durations_with_decoder(*decoder, ideal_size, bitmaps, result.durations);

    if (bitmaps.is_empty())
        return Error::from_string_literal("Failed to decode image frames");

    result.bitmaps = Gfx::BitmapSequence { move(bitmaps) };
    return result;
}

NonnullRefPtr<ConnectionFromClient::Job> ConnectionFromClient::make_decode_image_job(
    i64 image_id,
    Core::AnonymousBuffer encoded_buffer,
    Optional<Gfx::IntSize> ideal_size,
    Optional<ByteString> mime_type)
{
    auto job = Job::construct(
        [encoded_buffer = move(encoded_buffer), ideal_size = move(ideal_size), mime_type = move(mime_type)](auto&) -> ErrorOr<DecodeResult> {
            return decode_image_to_details(encoded_buffer, ideal_size, mime_type);
        },
        [strong_this = NonnullRefPtr(*this), image_id](DecodeResult result) -> ErrorOr<void> {
            if (!strong_this->is_open())
                return {};

            strong_this->async_did_decode_image(
                image_id,
                result.is_animated,
                result.loop_count,
                move(result.bitmaps),
                move(result.durations),
                result.scale,
                move(result.color_profile));

            strong_this->m_pending_jobs.remove(image_id);
            return {};
        },
        [strong_this = NonnullRefPtr(*this), image_id](Error error) {
            if (strong_this->is_open())
                strong_this->async_did_fail_to_decode_image(
                    image_id,
                    MUST(String::formatted("Decoding failed: {}", error)));

            strong_this->m_pending_jobs.remove(image_id);
        });

    // SECURITY: Decode timeout watchdog
    Core::Timer::create_single_shot(DECODE_TIMEOUT_MS, [weak_receiver = job->make_weak_ptr<Core::EventReceiver>()]() {
        if (auto ref = weak_receiver.strong_ref()) {
            auto job_ref = AK::static_ptr_cast<Job>(ref);
            if (job_ref)
                job_ref->cancel();
        }
    });

    return job;
}

Messages::ImageDecoderServer::DecodeImageResponse ConnectionFromClient::decode_image(
    Core::AnonymousBuffer encoded_buffer,
    Optional<Gfx::IntSize> ideal_size,
    Optional<ByteString> mime_type)
{
    auto image_id = m_next_image_id++;

    // SECURITY: Concurrent job limit
    if (m_pending_jobs.size() >= MAX_PENDING_JOBS) {
        async_did_fail_to_decode_image(image_id, "Too many concurrent decode requests"_string);
        return image_id;
    }

    if (!encoded_buffer.is_valid()) {
        async_did_fail_to_decode_image(image_id, "Invalid encoded buffer"_string);
        return image_id;
    }

    // SECURITY: Input size limit
    if (encoded_buffer.size() > MAX_ENCODED_IMAGE_SIZE) {
        async_did_fail_to_decode_image(image_id, "Image too large"_string);
        return image_id;
    }

    m_pending_jobs.set(
        image_id,
        make_decode_image_job(image_id, move(encoded_buffer), ideal_size, move(mime_type)));

    return image_id;
}

void ConnectionFromClient::cancel_decoding(i64 image_id)
{
    if (auto job = m_pending_jobs.take(image_id); job.has_value())
        job.value()->cancel();
}

}
