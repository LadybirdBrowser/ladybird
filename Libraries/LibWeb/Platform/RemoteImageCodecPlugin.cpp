/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibIPC/TransportHandle.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWeb/Platform/RemoteImageCodecPlugin.h>

namespace Web::Platform {

// NB: A decoder is a process of its own, with the memory it keeps, while connecting to a new one when the next image comes
//     along takes a few milliseconds. So a client that has had nothing to do for this long is let go of.
static constexpr AK::Duration idle_time_before_releasing_image_decoder = AK::Duration::from_seconds(30);

RemoteImageCodecPlugin::RemoteImageCodecPlugin(ConnectToImageDecoder connect_to_image_decoder, ReleaseImageDecoder release_image_decoder)
    : m_connect_to_image_decoder(move(connect_to_image_decoder))
    , m_release_image_decoder(move(release_image_decoder))
    , m_idle_timer(Core::Timer::create_single_shot(idle_time_before_releasing_image_decoder, [this] { release_idle_client(); }))
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
        m_idle_timer->stop();
    };
    return m_client;
}

void RemoteImageCodecPlugin::client_may_have_become_idle()
{
    if (m_release_image_decoder && m_client && m_client->is_idle())
        m_idle_timer->restart();
}

// NB: Only this process asks its client for anything, so a client that is idle here has nothing in flight, and letting
//     go of it loses nothing. The next image to decode connects a new one.
void RemoteImageCodecPlugin::release_idle_client()
{
    if (!m_client || !m_client->is_idle())
        return;
    disconnect_client();
    m_release_image_decoder();
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
        [this, promise](ImageDecoderClient::DecodedImage& result) -> ErrorOr<void> {
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
            client_may_have_become_idle();
            return {};
        },
        [this, promise](auto& error) {
            promise->reject(Error::copy(error));
            client_may_have_become_idle();
        });

    return promise;
}

ErrorOr<DecodedFirstFrame> RemoteImageCodecPlugin::decode_first_frame_synchronously(ReadonlyBytes bytes)
{
    auto* client = ensure_client();
    if (!client)
        return Error::from_string_literal("ImageDecoderClient is disconnected");
    auto result = client->decode_first_frame_synchronously(bytes);
    client_may_have_become_idle();
    auto frame = TRY(move(result));
    return DecodedFirstFrame { move(frame.bitmap), move(frame.color_space) };
}

void RemoteImageCodecPlugin::stop_animation_decode(i64 session_id)
{
    if (!m_client)
        return;
    m_client->stop_animation_decode(session_id);
    client_may_have_become_idle();
}

void RemoteImageCodecPlugin::connect_frame_client(IPC::TransportHandle handle)
{
    auto* client = ensure_client();
    if (!client)
        return;
    client->async_connect_frame_client(handle);
    // NB: This may have connected a client that has nothing to do, which must be let go of like any other.
    client_may_have_become_idle();
}

}
