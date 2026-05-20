/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Forward.h>

class SkImage;

template<typename T>
class sk_sp;

namespace Gfx {

class DecodedImageFrameSkiaImageCache final {
public:
    DecodedImageFrameSkiaImageCache();
    explicit DecodedImageFrameSkiaImageCache(RefPtr<SkiaBackendContext>);
    ~DecodedImageFrameSkiaImageCache();

    sk_sp<SkImage> image_for_frame(DecodedImageFrame const&);
    void prune();

private:
    struct Impl;
    OwnPtr<Impl> m_impl;
};

}
