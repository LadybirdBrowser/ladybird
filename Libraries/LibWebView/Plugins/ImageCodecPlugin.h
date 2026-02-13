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

    virtual NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(Web::Platform::DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override;

    virtual void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count) override;
    virtual void stop_animation_decode(i64 session_id) override;

    void set_client(NonnullRefPtr<ImageDecoderClient::Client>);

private:
    void setup_client_callbacks();

    RefPtr<ImageDecoderClient::Client> m_client;
};

}
