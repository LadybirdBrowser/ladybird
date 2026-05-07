/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <core/SkRefCnt.h>

class SkColorSpace;
class SkImage;

namespace Gfx {

class DecodedImageFrameSkiaImageCache final {
public:
    DecodedImageFrameSkiaImageCache();
    explicit DecodedImageFrameSkiaImageCache(RefPtr<SkiaBackendContext>);
    ~DecodedImageFrameSkiaImageCache();

    sk_sp<SkImage> image_for_frame(DecodedImageFrame const&);
    void prune();

private:
    struct DecodedImageFrameKeyTraits : public Traits<DecodedImageFrame> {
        static unsigned hash(DecodedImageFrame const& frame)
        {
            return pair_int_hash(
                ptr_hash(&frame.bitmap()),
                ptr_hash(color_space_pointer(frame)));
        }

        static bool equals(DecodedImageFrame const& a, DecodedImageFrame const& b)
        {
            return &a.bitmap() == &b.bitmap()
                && color_space_pointer(a) == color_space_pointer(b);
        }

        static constexpr bool may_have_slow_equality_check() { return false; }

    private:
        static SkColorSpace const* color_space_pointer(DecodedImageFrame const& frame)
        {
            return frame.color_space().color_space<sk_sp<SkColorSpace>>().get();
        }
    };

    struct CachedImage {
        sk_sp<SkImage> image;
        u64 last_used_generation { 0 };
    };

    RefPtr<SkiaBackendContext> m_skia_backend_context;
    HashMap<DecodedImageFrame, CachedImage, DecodedImageFrameKeyTraits> m_images;
    u64 m_generation { 0 };
};

}
