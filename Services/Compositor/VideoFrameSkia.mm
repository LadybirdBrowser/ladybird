/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/VideoFrameSkia.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/VideoSurfaceImage.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAInfo.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/GrYUVABackendTextures.h>
#include <gpu/ganesh/SkImageGanesh.h>
#include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#include <gpu/ganesh/mtl/GrMtlTypes.h>
#include <ports/SkCFObject.h>

namespace Compositor {

sk_sp<SkImage> sk_image_from_video_surface(Core::IOSurfaceHandle const& io_surface, Media::CodingIndependentCodePoints cicp, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto* gr_context = skia_backend_context.sk_context();
    if (!gr_context)
        return nullptr;

    auto bit_depth = Gfx::biplanar_bit_depth_for_pixel_format(io_surface.pixel_format());
    if (!bit_depth.has_value() || io_surface.plane_count() != 2)
        return nullptr;

    Array plane_formats { Gfx::MetalTextureFormat::R8, Gfx::MetalTextureFormat::RG8 };
    // Samples deeper than 8 bits sit in the high bits of each 16-bit component, so they read as full range unexpanded.
    if (*bit_depth > 8)
        plane_formats = { Gfx::MetalTextureFormat::R16, Gfx::MetalTextureFormat::RG16 };
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

    auto color_space = Gfx::ColorSpace {};
    if (auto color_space_result = Gfx::ColorSpace::from_cicp(cicp); !color_space_result.is_error())
        color_space = color_space_result.release_value();

    GrYUVABackendTextures yuva_textures { yuva_info, plane_textures, kTopLeft_GrSurfaceOrigin };
    return SkImages::TextureFromYUVATextures(gr_context, yuva_textures, Gfx::to_skia_color_space(color_space));
}

}
