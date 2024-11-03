/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ImageCodecPlugin.h"
#include "Utilities.h"
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibImageDecoderClient/Client.h>

namespace Ladybird {

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

NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> ImageCodecPlugin::decode_image(ReadonlyBytes bytes, Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected)
{
    auto promise = Core::Promise<Web::Platform::DecodedImage>::construct();
    if (on_resolved)
        promise->on_resolution = move(on_resolved);
    if (on_rejected)
        promise->on_rejection = move(on_rejected);

    if (!m_client) {
        promise->reject(Error::from_string_literal("ImageDecoderClient is disconnected"));
        return promise;
    }

    auto image_decoder_promise = m_client->decode_image(
        bytes,
        [promise](ImageDecoderClient::DecodedImage& result) -> ErrorOr<void> {
            // FIXME: Remove this codec plugin and just use the ImageDecoderClient directly to avoid these copies
            Web::Platform::DecodedImage decoded_image;
            decoded_image.is_animated = result.is_animated;
            decoded_image.loop_count = result.loop_count;
            for (auto& frame : result.frames) {
                decoded_image.frames.empend(move(frame.bitmap), frame.duration);
            }
            promise->resolve(move(decoded_image));
            return {};
        },
        [promise](auto& error) {
            promise->reject(Error::copy(error));
        });

    return promise;
}

}
