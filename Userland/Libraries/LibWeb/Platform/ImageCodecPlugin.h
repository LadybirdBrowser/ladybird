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
#include <LibGfx/Forward.h>

namespace Web::Platform {

struct Frame {
    RefPtr<Gfx::Bitmap> bitmap;
    size_t duration { 0 };
};

struct DecodedImage {
    bool is_animated { false };
    u32 loop_count { 0 };
    Vector<Frame> frames;
};

class ImageCodecPlugin {
public:
    static ImageCodecPlugin& the();
    static void install(ImageCodecPlugin&);

    virtual ~ImageCodecPlugin();

    virtual NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, ESCAPING Function<ErrorOr<void>(DecodedImage&)> on_resolved, ESCAPING Function<void(Error&)> on_rejected) = 0;
};

}
