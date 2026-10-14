/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Forward.h>

class SkImage;
class SkYUVAPixmaps;
enum SkYUVColorSpace : int;

template<typename T>
class sk_sp;

namespace Compositor {

SkYUVColorSpace skia_yuv_color_space(Media::CodingIndependentCodePoints);

// Views the planes of the YUV data as Skia pixmaps. Samples deeper than 8 bits are copied and stretched to 16 bits.
SkYUVAPixmaps make_yuva_pixmaps(Gfx::YUVData const&);

// Views a decoder's surface as a GPU image, sampling its planes where they already are instead of uploading a copy.
// Returns null for a surface whose layout is not one the platform decoder produces, or on a platform that cannot
// sample its surfaces yet.
sk_sp<SkImage> sk_image_from_video_surface(Media::VideoSurface const&, Media::CodingIndependentCodePoints, Gfx::SkiaBackendContext&);

}
