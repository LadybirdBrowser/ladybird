/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_MACOS

#    include <AK/Optional.h>
#    include <AK/Vector.h>
#    include <LibCore/IOSurface.h>
#    include <LibGfx/Forward.h>
#    include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#    include <LibMediaCodecs/Subsampling.h>

namespace Gfx {

// The formats a platform decoder hands back hold luma alone in the first plane and the two chroma components
// interleaved in the second. An RGB picture is held in the same layout, with green as luma, then blue and red.
struct BiplanarPixelFormat {
    u32 pixel_format;
    u8 bit_depth;
    Media::Subsampling subsampling;
    Media::VideoFullRangeFlag range;
};

Optional<BiplanarPixelFormat> biplanar_pixel_format(u32 pixel_format);

// The formats that hold a picture of the given bit depth and subsampling, one per range. Empty if there are none.
Vector<BiplanarPixelFormat, 2> biplanar_pixel_formats_holding(u8 bit_depth, Media::Subsampling);

ErrorOr<void> validate_biplanar_video_surface(Core::IOSurfaceHandle const&);

// Reads a decoder's surface back into RGBA pixels, for the paths that need them in memory rather than on the GPU.
ErrorOr<NonnullRefPtr<Bitmap>> bitmap_from_video_surface(Core::IOSurfaceHandle const&, Media::CodingIndependentCodePoints);

}

#endif
