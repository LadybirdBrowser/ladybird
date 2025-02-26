/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibImageDecoderClient/Client.h>

namespace ImageDecoderClient {

Client::Client(IPC::Transport transport)
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

void Client::did_decode_image(i64 image_id, bool is_animated, u32 loop_count, Gfx::BitmapSequence bitmap_sequence, Vector<u32> durations, Gfx::FloatPoint scale, Gfx::ColorSpace color_space)
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
    image.scale = scale;
    image.frames.ensure_capacity(bitmaps.size());
    image.color_space = move(color_space);
    for (size_t i = 0; i < bitmaps.size(); ++i) {
        if (!bitmaps[i]) {
            dbgln("ImageDecoderClient: Invalid bitmap for request {} at index {}", image_id, i);
            promise->reject(Error::from_string_literal("Invalid bitmap"));
            return;
        }

        image.frames.empend(bitmaps[i].release_nonnull(), durations[i]);
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

}
