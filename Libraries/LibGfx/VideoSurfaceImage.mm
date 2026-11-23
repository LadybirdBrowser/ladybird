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

static constexpr Array biplanar_pixel_formats = {
    BiplanarPixelFormat { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 8, Media::Subsampling::yuv420(), Media::VideoFullRangeFlag::Studio },
    BiplanarPixelFormat { kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 8, Media::Subsampling::yuv420(), Media::VideoFullRangeFlag::Full },
    BiplanarPixelFormat { kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, 10, Media::Subsampling::yuv420(), Media::VideoFullRangeFlag::Studio },
    BiplanarPixelFormat { kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, 10, Media::Subsampling::yuv420(), Media::VideoFullRangeFlag::Full },
    BiplanarPixelFormat { kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, 8, Media::Subsampling::yuv422(), Media::VideoFullRangeFlag::Studio },
    BiplanarPixelFormat { kCVPixelFormatType_422YpCbCr8BiPlanarFullRange, 8, Media::Subsampling::yuv422(), Media::VideoFullRangeFlag::Full },
    BiplanarPixelFormat { kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, 10, Media::Subsampling::yuv422(), Media::VideoFullRangeFlag::Studio },
    BiplanarPixelFormat { kCVPixelFormatType_422YpCbCr10BiPlanarFullRange, 10, Media::Subsampling::yuv422(), Media::VideoFullRangeFlag::Full },
    BiplanarPixelFormat { kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange, 8, Media::Subsampling::yuv444(), Media::VideoFullRangeFlag::Studio },
    BiplanarPixelFormat { kCVPixelFormatType_444YpCbCr8BiPlanarFullRange, 8, Media::Subsampling::yuv444(), Media::VideoFullRangeFlag::Full },
    BiplanarPixelFormat { kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, 10, Media::Subsampling::yuv444(), Media::VideoFullRangeFlag::Studio },
    BiplanarPixelFormat { kCVPixelFormatType_444YpCbCr10BiPlanarFullRange, 10, Media::Subsampling::yuv444(), Media::VideoFullRangeFlag::Full },
};

Optional<BiplanarPixelFormat> biplanar_pixel_format(u32 pixel_format)
{
    for (auto const& format : biplanar_pixel_formats) {
        if (format.pixel_format == pixel_format)
            return format;
    }
    return {};
}

Vector<BiplanarPixelFormat, 2> biplanar_pixel_formats_holding(u8 bit_depth, Media::Subsampling subsampling)
{
    // A picture deeper than any format is held in the deepest one.
    auto format_bit_depth = bit_depth > 8 ? 10 : 8;
    Vector<BiplanarPixelFormat, 2> formats;
    for (auto const& format : biplanar_pixel_formats) {
        if (format.bit_depth == format_bit_depth && format.subsampling == subsampling)
            formats.append(format);
    }
    return formats;
}

ErrorOr<void> validate_biplanar_video_surface(Core::IOSurfaceHandle const& io_surface)
{
    auto format = biplanar_pixel_format(io_surface.pixel_format());
    if (!format.has_value() || io_surface.plane_count() != 2)
        return Error::from_string_literal("Video surface is not in a supported bi-planar format");

    auto width = io_surface.plane_width(0);
    auto height = io_surface.plane_height(0);
    if (width == 0 || height == 0 || width > Media::MAX_VIDEO_FRAME_DIMENSION || height > Media::MAX_VIDEO_FRAME_DIMENSION)
        return Error::from_string_literal("Video surface has an unsupported size");

    size_t bytes_per_sample = format->bit_depth > 8 ? 2 : 1;
    auto chroma_size = format->subsampling.subsampled_size(Size<size_t> { width, height });
    auto chroma_width = chroma_size.width();
    auto chroma_height = chroma_size.height();
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
    auto format = biplanar_pixel_format(io_surface.pixel_format());
    if (!format.has_value() || io_surface.plane_count() != 2)
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
    return biplanar_yuv_to_bitmap(size, format->bit_depth, format->subsampling, cicp, plane(0), plane(1));
}

}
