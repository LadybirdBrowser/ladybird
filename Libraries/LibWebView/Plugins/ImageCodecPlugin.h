/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibImageDecoderClient/Client.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API ImageCodecPlugin final : public Web::Platform::ImageCodecPlugin {
public:
    explicit ImageCodecPlugin(NonnullRefPtr<ImageDecoderClient::Client>);
    virtual ~ImageCodecPlugin() override;

    virtual PendingDecode start_decoding_image(Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override;
    virtual void partial_image_data_became_available(PendingDecode const& pending_decode, ReadonlyBytes encoded_data) override;
    virtual void no_more_data_for_image(PendingDecode const& pending_decode) override;

    void set_client(NonnullRefPtr<ImageDecoderClient::Client>);

private:
    RefPtr<ImageDecoderClient::Client> m_client;
};

}
