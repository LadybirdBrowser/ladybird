/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWeb/Export.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::Platform {

class WEB_API RemoteImageCodecPlugin final : public ImageCodecPlugin {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit RemoteImageCodecPlugin(NonnullRefPtr<ImageDecoderClient::Client>);
    virtual ~RemoteImageCodecPlugin() override;

    virtual NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override;

    virtual void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count) override;
    virtual void stop_animation_decode(i64 session_id) override;

    void set_client(NonnullRefPtr<ImageDecoderClient::Client>);

private:
    void setup_client_callbacks();

    RefPtr<ImageDecoderClient::Client> m_client;
};

}
