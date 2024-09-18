/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <ImageDecoder/ImageDecoderServerEndpoint.h>
#include <LibCore/Promise.h>
#include <LibIPC/ConnectionToServer.h>

namespace ImageDecoderClient {

struct Frame {
    NonnullRefPtr<Gfx::Bitmap> bitmap;
    u32 duration { 0 };
};

struct DecodedImage {
    bool is_animated { false };
    Gfx::FloatPoint scale { 1, 1 };
    u32 loop_count { 0 };
    Vector<Frame> frames;
};

class Client final
    : public IPC::ConnectionToServer<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint>
    , public ImageDecoderClientEndpoint {
    IPC_CLIENT_CONNECTION(Client, "/tmp/session/%sid/portal/image"sv);

public:
    Client(NonnullOwnPtr<Core::LocalSocket>);

    NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected, Optional<Gfx::IntSize> ideal_size = {}, Optional<ByteString> mime_type = {});

    Function<void()> on_death;

private:
    virtual void die() override;

    virtual void did_decode_image(i64 image_id, bool is_animated, u32 loop_count, Gfx::BitmapSequence const& bitmap_sequence, Vector<u32> const& durations, Gfx::FloatPoint scale) override;
    virtual void did_fail_to_decode_image(i64 image_id, String const& error_message) override;

    HashMap<i64, NonnullRefPtr<Core::Promise<DecodedImage>>> m_pending_decoded_images;
};

}
