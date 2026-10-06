/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/VideoSurfaceImage.h>
#include <LibGfx/YUVData.h>

#include <CoreVideo/CoreVideo.h>

namespace Gfx {

Optional<u8> biplanar_bit_depth_for_pixel_format(u32 pixel_format)
{
    switch (pixel_format) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        return 8;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        return 10;
    default:
        return {};
    }
}

ErrorOr<NonnullRefPtr<Bitmap>> bitmap_from_video_surface(Core::IOSurfaceHandle const& io_surface, Media::CodingIndependentCodePoints cicp)
{
    auto bit_depth = biplanar_bit_depth_for_pixel_format(io_surface.pixel_format());
    if (!bit_depth.has_value() || io_surface.plane_count() != 2)
        return Error::from_string_literal("Unsupported video surface pixel format");

    if (!io_surface.lock_read_only())
        return Error::from_string_literal("Could not lock a video surface to read it");
    ScopeGuard unlock_surface = [&] { io_surface.unlock_read_only(); };

    auto plane = [&](size_t index) {
        auto stride = io_surface.bytes_per_row_of_plane(index);
        return BiplanarYUVPlane {
            .data = { io_surface.data_of_plane(index), stride * io_surface.plane_height(index) },
            .stride = stride,
        };
    };

    IntSize size { static_cast<int>(io_surface.plane_width(0)), static_cast<int>(io_surface.plane_height(0)) };
    return biplanar_yuv_to_bitmap(size, *bit_depth, cicp, plane(0), plane(1));
}

}
