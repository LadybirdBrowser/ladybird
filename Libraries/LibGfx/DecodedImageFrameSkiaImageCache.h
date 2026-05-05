/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <core/SkRefCnt.h>

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
    struct CachedImage {
        RefPtr<DecodedImageFrame const> frame;
        sk_sp<SkImage> image;
        u64 last_used_generation { 0 };
    };

    RefPtr<SkiaBackendContext> m_skia_backend_context;
    HashMap<DecodedImageFrame const*, CachedImage> m_images;
    u64 m_generation { 0 };
};

}
