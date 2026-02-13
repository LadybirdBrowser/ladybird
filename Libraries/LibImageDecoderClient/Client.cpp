/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibImageDecoderClient/Client.h>

namespace ImageDecoderClient {

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint>(*this, move(transport))
{
}

void Client::die()
{
    for (auto& [_, promise] : m_pending_decoded_images) {
        promise->reject(Error::from_string_literal("ImageDecoder disconnected"));
    }
    m_pending_decoded_images.clear();
}

NonnullRefPtr<Core::Promise<DecodedImage>> Client::decode_image(ReadonlyBytes encoded_data, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type)
{
    auto promise = Core::Promise<DecodedImage>::construct();
    if (on_resolved)
        promise->on_resolution = move(on_resolved);
    if (on_rejected)
        promise->on_rejection = move(on_rejected);

    if (encoded_data.is_empty()) {
        promise->reject(Error::from_string_literal("No encoded data"));
        return promise;
    }

    auto encoded_buffer_or_error = Core::AnonymousBuffer::create_with_size(encoded_data.size());
    if (encoded_buffer_or_error.is_error()) {
        dbgln("Could not allocate encoded buffer: {}", encoded_buffer_or_error.error());
        promise->reject(encoded_buffer_or_error.release_error());
        return promise;
    }
    auto encoded_buffer = encoded_buffer_or_error.release_value();

    memcpy(encoded_buffer.data<void>(), encoded_data.data(), encoded_data.size());

    auto response = send_sync_but_allow_failure<Messages::ImageDecoderServer::DecodeImage>(move(encoded_buffer), ideal_size, mime_type);
    if (!response) {
        dbgln("ImageDecoder disconnected trying to decode image");
        promise->reject(Error::from_string_literal("ImageDecoder disconnected"));
        return promise;
    }

    m_pending_decoded_images.set(response->image_id(), promise);

    return promise;
}

void Client::did_decode_image(i64 image_id, bool is_animated, u32 loop_count, Gfx::BitmapSequence bitmap_sequence, Vector<u32> durations, Gfx::FloatPoint scale, Gfx::ColorSpace color_space, i64 session_id)
{
    auto bitmaps = move(bitmap_sequence.bitmaps);
    VERIFY(!bitmaps.is_empty());

    auto maybe_promise = m_pending_decoded_images.take(image_id);
    if (!maybe_promise.has_value()) {
        dbgln("ImageDecoderClient: No pending image with ID {}", image_id);
        return;
    }
    auto promise = maybe_promise.release_value();

    DecodedImage image;
    image.is_animated = is_animated;
    image.loop_count = loop_count;
    image.session_id = session_id;
    image.scale = scale;
    image.frames.ensure_capacity(bitmaps.size());
    image.color_space = move(color_space);

    if (session_id != 0) {
        // Streaming animated decode: durations contains ALL frame durations,
        // but bitmaps only contains the first batch.
        image.frame_count = durations.size();
        image.all_durations = move(durations);
    }

    for (size_t i = 0; i < bitmaps.size(); ++i) {
        if (!bitmaps[i]) {
            dbgln("ImageDecoderClient: Invalid bitmap for request {} at index {}", image_id, i);
            promise->reject(Error::from_string_literal("Invalid bitmap"));
            return;
        }

        u32 duration = (session_id != 0) ? image.all_durations[i] : durations[i];
        image.frames.empend(bitmaps[i].release_nonnull(), duration);
    }

    promise->resolve(move(image));
}

void Client::did_fail_to_decode_image(i64 image_id, String error_message)
{
    auto maybe_promise = m_pending_decoded_images.take(image_id);
    if (!maybe_promise.has_value()) {
        dbgln("ImageDecoderClient: No pending image with ID {}", image_id);
        return;
    }
    auto promise = maybe_promise.release_value();

    dbgln("ImageDecoderClient: Failed to decode image with ID {}: {}", image_id, error_message);
    // FIXME: Include the error message in the Error object when Errors are allowed to hold Strings
    promise->reject(Error::from_string_literal("Image decoding failed or aborted"));
}

void Client::did_decode_animation_frames(i64 session_id, Gfx::BitmapSequence bitmap_sequence)
{
    if (!on_animation_frames_decoded)
        return;

    auto bitmaps = move(bitmap_sequence.bitmaps);
    Vector<NonnullRefPtr<Gfx::Bitmap>> result;
    result.ensure_capacity(bitmaps.size());
    for (auto& bitmap : bitmaps) {
        if (bitmap)
            result.unchecked_append(bitmap.release_nonnull());
    }
    on_animation_frames_decoded(session_id, move(result));
}

void Client::did_fail_animation_decode(i64 session_id, String error_message)
{
    if (on_animation_decode_failed)
        on_animation_decode_failed(session_id, move(error_message));
}

void Client::request_animation_frames(i64 session_id, u32 start_frame_index, u32 count)
{
    async_request_animation_frames(session_id, start_frame_index, count);
}

void Client::stop_animation_decode(i64 session_id)
{
    async_stop_animation_decode(session_id);
}

}
