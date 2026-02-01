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
    Vector<Frame> frames;
    Gfx::ColorSpace color_space;
};

class WEB_API ImageCodecPlugin {
public:
    static ImageCodecPlugin& the();
    static void install(ImageCodecPlugin&);

    virtual ~ImageCodecPlugin();

    struct PendingDecode {
        i64 image_id { 0 };
        NonnullRefPtr<Core::Promise<Web::Platform::DecodedImage>> promise;
    };

    virtual PendingDecode start_decoding_image(ESCAPING Function<ErrorOr<void>(DecodedImage&)> on_resolved, ESCAPING Function<void(Error&)> on_rejected) = 0;
    virtual void partial_image_data_became_available(PendingDecode const& pending_decode, ReadonlyBytes encoded_data) = 0;
    virtual void no_more_data_for_image(PendingDecode const& pending_decode) = 0;
};

}
