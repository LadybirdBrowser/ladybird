/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <ImageDecoder/Forward.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <ImageDecoder/ImageDecoderServerEndpoint.h>
#include <LibGfx/BitmapSequence.h>
#include <LibGfx/ColorSpace.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibThreading/BackgroundAction.h>

namespace ImageDecoder {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override = default;

    virtual void die() override;

    struct DecodeResult {
        bool is_animated = false;
        u32 loop_count = 0;
        Gfx::FloatPoint scale { 1, 1 };
        Gfx::BitmapSequence bitmaps;
        Vector<u32> durations;
        Gfx::ColorSpace color_profile;
    };

private:
    using Job = Threading::BackgroundAction<DecodeResult>;

    explicit ConnectionFromClient(IPC::Transport);

    virtual Messages::ImageDecoderServer::DecodeImageResponse decode_image(Core::AnonymousBuffer const&, Optional<Gfx::IntSize> const& ideal_size, Optional<ByteString> const& mime_type) override;
    virtual void cancel_decoding(i64 image_id) override;
    virtual Messages::ImageDecoderServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;
    virtual Messages::ImageDecoderServer::InitTransportResponse init_transport(int peer_pid) override;

    ErrorOr<IPC::File> connect_new_client();

    NonnullRefPtr<Job> make_decode_image_job(i64 image_id, Core::AnonymousBuffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type);

    i64 m_next_image_id { 0 };
    HashMap<i64, NonnullRefPtr<Job>> m_pending_jobs;
};

}
