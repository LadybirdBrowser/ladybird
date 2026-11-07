/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibIPC/TransportHandle.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWeb/Platform/RemoteImageCodecPlugin.h>

namespace Web::Platform {

RemoteImageCodecPlugin::RemoteImageCodecPlugin(ConnectToImageDecoder connect_to_image_decoder)
    : m_connect_to_image_decoder(move(connect_to_image_decoder))
{
}

ImageDecoderClient::Client* RemoteImageCodecPlugin::ensure_client()
{
    if (m_client)
        return m_client;

    auto client = m_connect_to_image_decoder();
    if (client.is_error()) {
        dbgln("Failed to connect to an image decoder: {}", client.error());
        return nullptr;
    }

    // NB: The generation also changes here, for a frame channel that was given up on while no client could be had.
    m_client = client.release_value();
    ++m_client_generation;
    m_client->on_death = [this] {
        m_client = nullptr;
        ++m_client_generation;
    };
    return m_client;
}

void RemoteImageCodecPlugin::disconnect_client()
{
    if (auto client = m_client)
        client->shutdown();
}

RemoteImageCodecPlugin::~RemoteImageCodecPlugin() = default;

NonnullRefPtr<Core::Promise<DecodedImage>> RemoteImageCodecPlugin::decode_image(ReadonlyBytes bytes, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected)
{
    auto promise = Core::Promise<DecodedImage>::construct();
    if (on_resolved)
        promise->on_resolution = move(on_resolved);
    if (on_rejected)
        promise->on_rejection = move(on_rejected);

    auto* client = ensure_client();
    if (!client) {
        promise->reject(Error::from_string_literal("ImageDecoderClient is disconnected"));
        return promise;
    }

    auto image_decoder_promise = client->decode_image(
        bytes,
        [promise](ImageDecoderClient::DecodedImage& result) -> ErrorOr<void> {
            // FIXME: Remove this codec plugin and just use the ImageDecoderClient directly to avoid these copies
            DecodedImage decoded_image;
            decoded_image.is_animated = result.is_animated;
            decoded_image.loop_count = result.loop_count;
            decoded_image.frame_count = result.frame_count;
            decoded_image.session_id = result.session_id;
            decoded_image.all_durations = move(result.all_durations);
            for (auto& frame : result.frames) {
                decoded_image.frames.empend(move(frame.bitmap), frame.duration);
            }
            decoded_image.color_space = move(result.color_space);
            promise->resolve(move(decoded_image));
            return {};
        },
        [promise](auto& error) {
            promise->reject(Error::copy(error));
        });

    return promise;
}

ErrorOr<DecodedFirstFrame> RemoteImageCodecPlugin::decode_first_frame_synchronously(ReadonlyBytes bytes)
{
    auto* client = ensure_client();
    if (!client)
        return Error::from_string_literal("ImageDecoderClient is disconnected");
    auto result = TRY(client->decode_first_frame_synchronously(bytes));
    return DecodedFirstFrame { move(result.bitmap), move(result.color_space) };
}

void RemoteImageCodecPlugin::stop_animation_decode(i64 session_id)
{
    if (m_client)
        m_client->stop_animation_decode(session_id);
}

void RemoteImageCodecPlugin::connect_frame_client(IPC::TransportHandle handle)
{
    if (auto* client = ensure_client())
        client->async_connect_frame_client(handle);
}

}
