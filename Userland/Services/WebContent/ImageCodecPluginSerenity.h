/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace ImageDecoderClient {
class Client;
}

namespace WebContent {

class ImageCodecPluginSerenity final : public Web::Platform::ImageCodecPlugin {
public:
    ImageCodecPluginSerenity();
    virtual ~ImageCodecPluginSerenity() override;

    virtual NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> decode_image(ReadonlyBytes, ESCAPING Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, ESCAPING Function<void(Error&)> on_rejected) override;

private:
    RefPtr<ImageDecoderClient::Client> m_client;
};

}
