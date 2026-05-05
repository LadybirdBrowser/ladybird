/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/DecodedImageFrameSkiaImageCache.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkImage.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Gfx {

static constexpr u64 image_cache_max_unused_generations = 120;

DecodedImageFrameSkiaImageCache::DecodedImageFrameSkiaImageCache() = default;

DecodedImageFrameSkiaImageCache::DecodedImageFrameSkiaImageCache(RefPtr<SkiaBackendContext> skia_backend_context)
    : m_skia_backend_context(move(skia_backend_context))
{
}

DecodedImageFrameSkiaImageCache::~DecodedImageFrameSkiaImageCache() = default;

sk_sp<SkImage> DecodedImageFrameSkiaImageCache::image_for_frame(DecodedImageFrame const& frame)
{
    if (auto it = m_images.find(&frame); it != m_images.end()) {
        it->value.last_used_generation = m_generation;
        return it->value.image;
    }

    auto raster_image = sk_image_from_bitmap(frame.bitmap(), frame.color_space());
    sk_sp<SkImage> image;
    auto* gr_context = m_skia_backend_context ? m_skia_backend_context->sk_context() : nullptr;
    if (gr_context) {
        image = SkImages::TextureFromImage(gr_context, raster_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
        if (!image)
            image = move(raster_image);
    } else {
        image = move(raster_image);
    }

    if (!image)
        return nullptr;

    CachedImage cached_image {
        .frame = &frame,
        .image = image,
        .last_used_generation = m_generation,
    };
    m_images.set(&frame, move(cached_image));
    return image;
}

void DecodedImageFrameSkiaImageCache::prune()
{
    m_images.remove_all_matching([this](auto const&, auto const& cached_image) {
        return m_generation - cached_image.last_used_generation > image_cache_max_unused_generations;
    });
    ++m_generation;
}

}
