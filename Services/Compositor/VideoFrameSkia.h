/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibGfx/Forward.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

class SkImage;
class SkYUVAPixmaps;
enum SkYUVColorSpace : int;

template<typename T>
class sk_sp;

namespace Compositor {

SkYUVColorSpace skia_yuv_color_space(Media::CodingIndependentCodePoints);

// Views the planes of the YUV data as Skia pixmaps. Samples deeper than 8 bits are copied and stretched to 16 bits.
SkYUVAPixmaps make_yuva_pixmaps(Gfx::YUVData const&);

#ifdef AK_OS_MACOS
// Views a decoder's surface as a GPU image, sampling its planes where they already are instead of uploading a copy.
// Returns null for a surface whose layout is not one the platform decoder produces.
sk_sp<SkImage> sk_image_from_video_surface(Core::IOSurfaceHandle const&, Media::CodingIndependentCodePoints, Gfx::SkiaBackendContext&);
#endif

}
