/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_MACOS

#    include <LibCore/IOSurface.h>
#    include <LibGfx/Forward.h>
#    include <LibMedia/Color/CodingIndependentCodePoints.h>
#    include <core/SkImage.h>

namespace Gfx {

class SkiaBackendContext;

// Views a decoder's surface as a GPU image, sampling its planes where they already are instead of uploading a copy.
// Returns null for a surface whose layout is not one the platform decoder produces.
sk_sp<SkImage> sk_image_from_video_surface(Core::IOSurfaceHandle const&, Media::CodingIndependentCodePoints, SkiaBackendContext&);

// Reads a decoder's surface back into RGBA pixels, for the paths that need them in memory rather than on the GPU.
ErrorOr<NonnullRefPtr<Bitmap>> bitmap_from_video_surface(Core::IOSurfaceHandle const&, Media::CodingIndependentCodePoints);

}

#endif
