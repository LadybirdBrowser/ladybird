/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <ImageDecoder/ImageDecoderServerEndpoint.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibGfx/ColorSpace.h>
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
    u32 frame_count { 0 };
    Vector<Frame> frames;
    Vector<u32> all_durations;
    Gfx::ColorSpace color_space;
    i64 session_id { 0 };
};

class Client final
    : public IPC::ConnectionToServer<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint>
    , public ImageDecoderClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::ImageDecoderServer::InitTransport;

    Client(NonnullOwnPtr<IPC::Transport>);

    NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected, Optional<Gfx::IntSize> ideal_size = {}, Optional<ByteString> mime_type = {});

    void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count);
    void stop_animation_decode(i64 session_id);

    Function<void()> on_death;
    Function<void(i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>>)> on_animation_frames_decoded;
    Function<void(i64 session_id, String error_message)> on_animation_decode_failed;

private:
    void verify_event_loop() const;
    virtual void die() override;

    virtual void did_decode_image(i64 image_id, bool is_animated, u32 loop_count, Gfx::BitmapSequence bitmap_sequence, Vector<u32> durations, Gfx::FloatPoint scale, Gfx::ColorSpace color_space, i64 session_id) override;
    virtual void did_fail_to_decode_image(i64 image_id, String error_message) override;

    virtual void did_decode_animation_frames(i64 session_id, Gfx::BitmapSequence bitmaps) override;
    virtual void did_fail_animation_decode(i64 session_id, String error_message) override;

    Core::EventLoop* m_creation_event_loop { &Core::EventLoop::current() };
    HashMap<i64, NonnullRefPtr<Core::Promise<DecodedImage>>> m_pending_decoded_images;
};

}
