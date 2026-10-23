/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/SkiaBackendContext.h>
#include <Compositor/SkiaUtils.h>
#include <Compositor/VideoFrameSkia.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/ImportedYCbCrVulkanImage.h>
#include <LibMediaCodecs/VideoSurface.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <gpu/MutableTextureState.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>
#include <gpu/ganesh/vk/GrVkBackendSurface.h>
#include <gpu/ganesh/vk/GrVkTypes.h>
#include <gpu/vk/VulkanMutableTextureState.h>

namespace Compositor {

// The decoder hands each frame over in the general layout, released to whichever device takes the image next.
static constexpr VkImageLayout HANDED_OVER_LAYOUT = VK_IMAGE_LAYOUT_GENERAL;
static constexpr u32 HANDED_OVER_QUEUE_FAMILY = VK_QUEUE_FAMILY_EXTERNAL;

static VkSamplerYcbcrModelConversion ycbcr_model_for(Media::CodingIndependentCodePoints cicp)
{
    switch (cicp.matrix_coefficients()) {
    case Media::MatrixCoefficients::BT470BG:
    case Media::MatrixCoefficients::BT601:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
    case Media::MatrixCoefficients::BT2020NonConstantLuminance:
    case Media::MatrixCoefficients::BT2020ConstantLuminance:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
    case Media::MatrixCoefficients::Identity:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    default:
        return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    }
}

// 4:2:0 video sites its chroma level with the first luma sample of each pair and between the two rows, where the
// format can sample it there.
static VkChromaLocation chroma_location(VkFormatFeatureFlags format_features, bool cosited)
{
    if (cosited && (format_features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT) != 0)
        return VK_CHROMA_LOCATION_COSITED_EVEN;
    if ((format_features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) != 0)
        return VK_CHROMA_LOCATION_MIDPOINT;
    return VK_CHROMA_LOCATION_COSITED_EVEN;
}

sk_sp<SkImage> sk_image_from_video_surface(Media::VideoSurface const& surface, Media::CodingIndependentCodePoints cicp, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto* gr_context = skia_backend_context.sk_context();
    if (!gr_context)
        return nullptr;
    auto const& vulkan_context = skia_backend_context.vulkan_context();
    if (!vulkan_context.can_sample_through_ycbcr_conversion)
        return nullptr;

    auto imported_or_error = Gfx::ImportedYCbCrVulkanImage::import_dma_buf(vulkan_context, surface.dma_buf());
    if (imported_or_error.is_error()) {
        dbgln("Could not import a video surface: {}", imported_or_error.error());
        return nullptr;
    }
    auto imported = imported_or_error.release_value();

    auto format_features = imported->format_features();
    auto chroma_filter = VK_FILTER_NEAREST;
    if ((format_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0)
        chroma_filter = VK_FILTER_LINEAR;
    auto range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    if (cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full)
        range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    skgpu::VulkanYcbcrConversionInfo ycbcr_info {
        imported->format(),
        ycbcr_model_for(cicp),
        range,
        chroma_location(format_features, true),
        chroma_location(format_features, false),
        chroma_filter,
        VK_FALSE,
        {},
        format_features,
    };

    // The image is only borrowed, so its memory is named for Skia's bookkeeping and never freed by it.
    auto const& memory = imported->memories().first();
    GrVkImageInfo image_info;
    image_info.fImage = imported->image();
    image_info.fAlloc.fMemory = memory.memory;
    image_info.fAlloc.fSize = memory.size;
    image_info.fImageTiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.fImageLayout = HANDED_OVER_LAYOUT;
    image_info.fFormat = imported->format();
    image_info.fImageUsageFlags = Gfx::ImportedYCbCrVulkanImage::USAGE;
    image_info.fLevelCount = 1;
    image_info.fCurrentQueueFamily = HANDED_OVER_QUEUE_FAMILY;
    image_info.fYcbcrConversionInfo = ycbcr_info;

    auto color_space = Gfx::ColorSpace {};
    if (auto color_space_result = Gfx::ColorSpace::from_cicp(cicp); !color_space_result.is_error())
        color_space = color_space_result.release_value();

    auto size = surface.dma_buf().size;
    auto backend_texture = GrBackendTextures::MakeVk(size.width(), size.height(), image_info);
    auto release_image = [](void* context) { delete static_cast<Gfx::ImportedYCbCrVulkanImage*>(context); };
    // Skia only wraps each YCbCr format as the color type with the same sample depth.
    auto color_type = kRGB_888x_SkColorType;
    if (imported->format() == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16)
        color_type = kRGBA_1010102_SkColorType;
    return SkImages::BorrowTextureFrom(gr_context, backend_texture, kTopLeft_GrSurfaceOrigin, color_type, kOpaque_SkAlphaType, Gfx::to_skia_color_space(color_space), release_image, imported.leak_ptr());
}

void video_surface_image_will_show_new_frame(SkImage& image)
{
    GrBackendTexture backend_texture;
    if (!SkImages::GetBackendTextureFromImage(&image, &backend_texture, false))
        return;
    backend_texture.setMutableState(skgpu::MutableTextureStates::MakeVulkan(HANDED_OVER_LAYOUT, HANDED_OVER_QUEUE_FAMILY));
}

}
