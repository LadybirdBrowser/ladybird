/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWebView/Plugins/ImageCodecPlugin.h>
#include <LibWebView/Utilities.h>

namespace WebView {

ImageCodecPlugin::ImageCodecPlugin(NonnullRefPtr<ImageDecoderClient::Client> client)
    : m_client(move(client))
{
    m_client->on_death = [this] {
        m_client = nullptr;
    };
}

void ImageCodecPlugin::set_client(NonnullRefPtr<ImageDecoderClient::Client> client)
{
    m_client = move(client);
    m_client->on_death = [this] {
        m_client = nullptr;
    };
}

ImageCodecPlugin::~ImageCodecPlugin() = default;

ImageCodecPlugin::PendingDecode ImageCodecPlugin::start_decoding_image(Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected)
{
    auto promise = Core::Promise<Web::Platform::DecodedImage>::construct();
    if (on_resolved)
        promise->on_resolution = move(on_resolved);
    if (on_rejected)
        promise->on_rejection = move(on_rejected);

    if (!m_client) {
        promise->reject(Error::from_string_literal("ImageDecoderClient is disconnected"));
        return PendingDecode {
            .image_id = -1,
            .promise = move(promise),
        };
    }

    auto image_decoder_in_flight_decoding = m_client->start_decoding_image(
        [promise](ImageDecoderClient::DecodedImage& result) -> ErrorOr<void> {
            // FIXME: Remove this codec plugin and just use the ImageDecoderClient directly to avoid these copies
            Web::Platform::DecodedImage decoded_image;
            decoded_image.is_animated = result.is_animated;
            decoded_image.loop_count = result.loop_count;
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

    promise->add_child(image_decoder_in_flight_decoding.promise);

    return PendingDecode {
        .image_id = image_decoder_in_flight_decoding.image_id,
        .promise = move(promise),
    };
}

void ImageCodecPlugin::partial_image_data_became_available(PendingDecode const& pending_decode, ReadonlyBytes encoded_data)
{
    if (!m_client) {
        pending_decode.promise->reject(Error::from_string_literal("ImageDecoderClient is disconnected"));
        return;
    }

    m_client->partial_image_data_became_available(pending_decode.image_id, encoded_data);
}

void ImageCodecPlugin::no_more_data_for_image(PendingDecode const& pending_decode)
{
    if (!m_client) {
        pending_decode.promise->reject(Error::from_string_literal("ImageDecoderClient is disconnected"));
        return;
    }

    m_client->no_more_data_for_image(pending_decode.image_id);
}

}
