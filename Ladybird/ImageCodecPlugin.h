/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibImageDecoderClient/Client.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Ladybird {

class ImageCodecPlugin final : public Web::Platform::ImageCodecPlugin {
public:
    explicit ImageCodecPlugin(NonnullRefPtr<ImageDecoderClient::Client>);
    virtual ~ImageCodecPlugin() override;

    virtual NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override;

    void set_client(NonnullRefPtr<ImageDecoderClient::Client>);

private:
    RefPtr<ImageDecoderClient::Client> m_client;
};

}
