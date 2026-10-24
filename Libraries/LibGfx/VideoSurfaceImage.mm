/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/VideoSurfaceImage.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/Limits.h>

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

ErrorOr<void> validate_biplanar_video_surface(Core::IOSurfaceHandle const& io_surface)
{
    auto bit_depth = biplanar_bit_depth_for_pixel_format(io_surface.pixel_format());
    if (!bit_depth.has_value() || io_surface.plane_count() != 2)
        return Error::from_string_literal("Video surface is not in a supported bi-planar format");

    auto width = io_surface.plane_width(0);
    auto height = io_surface.plane_height(0);
    if (width == 0 || height == 0 || width > Media::MAX_VIDEO_FRAME_DIMENSION || height > Media::MAX_VIDEO_FRAME_DIMENSION)
        return Error::from_string_literal("Video surface has an unsupported size");

    size_t bytes_per_sample = *bit_depth > 8 ? 2 : 1;
    auto chroma_width = (width + 1) / 2;
    auto chroma_height = (height + 1) / 2;
    struct PlaneExtent {
        size_t width;
        size_t height;
        size_t row_byte_count;
    };
    Array<PlaneExtent, 2> required_extents {
        PlaneExtent { width, height, width * bytes_per_sample },
        PlaneExtent { chroma_width, chroma_height, chroma_width * 2 * bytes_per_sample },
    };

    auto const* base = static_cast<u8 const*>(io_surface.data());
    for (size_t plane = 0; plane < required_extents.size(); plane++) {
        auto const& required = required_extents[plane];
        auto plane_height = io_surface.plane_height(plane);
        auto stride = io_surface.bytes_per_row_of_plane(plane);
        if (io_surface.plane_width(plane) < required.width || plane_height < required.height || stride < required.row_byte_count)
            return Error::from_string_literal("Video surface plane is smaller than its format requires");

        auto const* plane_base = static_cast<u8 const*>(io_surface.data_of_plane(plane));
        if (plane_base < base)
            return Error::from_string_literal("Video surface plane starts outside the surface");
        Checked<size_t> plane_byte_count = stride;
        plane_byte_count *= plane_height;
        Checked<size_t> plane_end = static_cast<size_t>(plane_base - base);
        plane_end += plane_byte_count;
        if (plane_end.has_overflow() || plane_end.value() > io_surface.alloc_size())
            return Error::from_string_literal("Video surface plane extends past the surface");
    }
    return {};
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
