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

class WEB_API ImageCodecPlugin {
public:
    static ImageCodecPlugin& the();
    static void install(ImageCodecPlugin&);

    virtual ~ImageCodecPlugin();

    virtual NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, ESCAPING Function<ErrorOr<void>(DecodedImage&)> on_resolved, ESCAPING Function<void(Error&)> on_rejected) = 0;

    virtual void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count) = 0;
    virtual void stop_animation_decode(i64 session_id) = 0;

    Function<void(i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>>)> on_animation_frames_decoded;
    Function<void(i64 session_id)> on_animation_decode_failed;
};

}
