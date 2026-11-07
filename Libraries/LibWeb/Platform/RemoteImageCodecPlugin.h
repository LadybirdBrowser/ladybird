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

    // Called for a client whenever one is needed and there is none, at first and after losing one.
    using ConnectToImageDecoder = Function<ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>>()>;
    // Called after a client that had nothing left to do was let go of, so that its decoder can end. Without it, a client
    // is kept for as long as its decoder is around.
    using ReleaseImageDecoder = Function<void()>;

    explicit RemoteImageCodecPlugin(ConnectToImageDecoder, ReleaseImageDecoder = {});
    virtual ~RemoteImageCodecPlugin() override;

    virtual NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override;

    virtual ErrorOr<DecodedFirstFrame> decode_first_frame_synchronously(ReadonlyBytes) override;

    virtual void stop_animation_decode(i64 session_id) override;
    virtual void connect_frame_client(IPC::TransportHandle) override;
    virtual u64 client_generation() const override { return m_client_generation; }

    // Gives up the client, as if its decoder had crashed. The next image to decode connects a new one.
    void disconnect_client();

private:
    ImageDecoderClient::Client* ensure_client();
    void client_may_have_become_idle();
    void release_idle_client();

    ConnectToImageDecoder m_connect_to_image_decoder;
    ReleaseImageDecoder m_release_image_decoder;
    NonnullRefPtr<Core::Timer> m_idle_timer;
    RefPtr<ImageDecoderClient::Client> m_client;
    u64 m_client_generation { 0 };
};

}
