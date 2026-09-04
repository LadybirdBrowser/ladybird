/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/VideoSurfaceImage.h>
#include <LibGfx/YUVData.h>

#include <core/SkColorSpace.h>
#include <core/SkYUVAInfo.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/GrYUVABackendTextures.h>
#include <gpu/ganesh/SkImageGanesh.h>
#include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#include <gpu/ganesh/mtl/GrMtlTypes.h>
#include <ports/SkCFObject.h>

#include <CoreVideo/CoreVideo.h>

namespace Gfx {

namespace {

// The formats a platform decoder hands back hold luma alone in the first plane and the two chroma components
// interleaved in the second.
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

}

sk_sp<SkImage> sk_image_from_video_surface(Core::IOSurfaceHandle const& io_surface, Media::CodingIndependentCodePoints cicp, SkiaBackendContext& skia_backend_context)
{
    auto* gr_context = skia_backend_context.sk_context();
    if (!gr_context)
        return nullptr;

    auto bit_depth = biplanar_bit_depth_for_pixel_format(io_surface.pixel_format());
    if (!bit_depth.has_value() || io_surface.plane_count() != 2)
        return nullptr;

    Array plane_formats { MetalTextureFormat::R8, MetalTextureFormat::RG8 };
    // Samples deeper than 8 bits sit in the high bits of each 16-bit component, so they read as full range unexpanded.
    if (*bit_depth > 8)
        plane_formats = { MetalTextureFormat::R16, MetalTextureFormat::RG16 };
    GrBackendTexture plane_textures[SkYUVAInfo::kMaxPlanes];
    for (size_t plane = 0; plane < plane_formats.size(); plane++) {
        auto metal_texture = skia_backend_context.metal_context().create_texture_from_iosurface(io_surface, plane_formats[plane], plane);
        if (!metal_texture)
            return nullptr;

        // The backend texture retains the Metal texture, so it outlives this handle to it.
        GrMtlTextureInfo texture_info;
        texture_info.fTexture = sk_ret_cfp(metal_texture->texture());
        plane_textures[plane] = GrBackendTextures::MakeMtl(
            static_cast<int>(metal_texture->width()), static_cast<int>(metal_texture->height()),
            skgpu::Mipmapped::kNo, texture_info);
    }

    SkYUVAInfo yuva_info {
        SkISize::Make(static_cast<int>(io_surface.plane_width(0)), static_cast<int>(io_surface.plane_height(0))),
        SkYUVAInfo::PlaneConfig::kY_UV,
        SkYUVAInfo::Subsampling::k420,
        skia_yuv_color_space(cicp),
    };

    auto color_space = ColorSpace {};
    if (auto color_space_result = ColorSpace::from_cicp(cicp); !color_space_result.is_error())
        color_space = color_space_result.release_value();

    GrYUVABackendTextures yuva_textures { yuva_info, plane_textures, kTopLeft_GrSurfaceOrigin };
    return SkImages::TextureFromYUVATextures(gr_context, yuva_textures, color_space.color_space<sk_sp<SkColorSpace>>());
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
