/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_MACOS

#    include <AK/Optional.h>
#    include <LibCore/IOSurface.h>
#    include <LibGfx/Forward.h>
#    include <LibMedia/Color/CodingIndependentCodePoints.h>

namespace Gfx {

// The formats a platform decoder hands back hold luma alone in the first plane and the two chroma components
// interleaved in the second. Returns the bit depth of a surface in one of these formats.
Optional<u8> biplanar_bit_depth_for_pixel_format(u32 pixel_format);

// Reads a decoder's surface back into RGBA pixels, for the paths that need them in memory rather than on the GPU.
ErrorOr<NonnullRefPtr<Bitmap>> bitmap_from_video_surface(Core::IOSurfaceHandle const&, Media::CodingIndependentCodePoints);

}

#endif
