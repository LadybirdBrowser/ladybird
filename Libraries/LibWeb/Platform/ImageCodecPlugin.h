/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/Promise.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>

namespace Web::Platform {

struct Frame {
    RefPtr<Gfx::Bitmap> bitmap;
    size_t duration { 0 };
};

struct DecodedImage {
    bool is_animated { false };
    u32 loop_count { 0 };
    u32 frame_count { 0 };
    Vector<Frame> frames;
    Vector<u32> all_durations;
    Gfx::ColorSpace color_space;
    i64 session_id { 0 };
};

struct DecodedFirstFrame {
    NonnullRefPtr<Gfx::Bitmap> bitmap;
    Gfx::ColorSpace color_space;
};

class WEB_API ImageCodecPlugin {
public:
    static ImageCodecPlugin& the();
    static bool is_initialized();
    static void install(ImageCodecPlugin&);

    virtual ~ImageCodecPlugin();

    virtual NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, ESCAPING Function<ErrorOr<void>(DecodedImage&)> on_resolved, ESCAPING Function<void(Error&)> on_rejected) = 0;

    // Decodes the first frame of an image while the caller waits, for a caller that needs the pixels of an image it
    // decoded before and has since discarded.
    virtual ErrorOr<DecodedFirstFrame> decode_first_frame_synchronously(ReadonlyBytes) = 0;

    virtual void stop_animation_decode(i64 session_id) = 0;

    // Has the decoder take the other end of the channel as a client that can ask for frames of this client's animation
    // sessions.
    virtual void connect_frame_client(IPC::TransportHandle) = 0;
    // Changes when the decoder is replaced, which takes its sessions and frame clients with it.
    virtual u64 client_generation() const = 0;
};

}
