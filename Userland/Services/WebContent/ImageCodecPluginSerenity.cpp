/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ImageCodecPluginSerenity.h"
#include <LibImageDecoderClient/Client.h>

namespace WebContent {

ImageCodecPluginSerenity::ImageCodecPluginSerenity() = default;
ImageCodecPluginSerenity::~ImageCodecPluginSerenity() = default;

NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> ImageCodecPluginSerenity::decode_image(ReadonlyBytes bytes, Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected)
{
    if (!m_client) {
        m_client = ImageDecoderClient::Client::try_create().release_value_but_fixme_should_propagate_errors();
        m_client->on_death = [&] {
            m_client = nullptr;
        };
    }

    auto promise = Core::Promise<Web::Platform::DecodedImage>::construct();
    if (on_resolved)
        promise->on_resolution = move(on_resolved);
    if (on_rejected)
        promise->on_rejection = move(on_rejected);

    auto image_decoder_promise = m_client->decode_image(
        bytes,
        [promise](ImageDecoderClient::DecodedImage& result) -> ErrorOr<void> {
            // FIXME: Remove this codec plugin and just use the ImageDecoderClient directly to avoid these copies
            Web::Platform::DecodedImage decoded_image;
            decoded_image.is_animated = result.is_animated;
            decoded_image.loop_count = result.loop_count;
            for (auto const& frame : result.frames) {
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
