/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ColorSpace.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/ImmutableBitmapSkiaImageCache.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/YUVData.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Gfx {

static constexpr u64 image_cache_max_unused_generations = 120;

ImmutableBitmapSkiaImageCache::ImmutableBitmapSkiaImageCache() = default;

ImmutableBitmapSkiaImageCache::ImmutableBitmapSkiaImageCache(RefPtr<SkiaBackendContext> skia_backend_context)
    : m_skia_backend_context(move(skia_backend_context))
{
}

ImmutableBitmapSkiaImageCache::~ImmutableBitmapSkiaImageCache() = default;

sk_sp<SkImage> ImmutableBitmapSkiaImageCache::image_for_bitmap(ImmutableBitmap const& bitmap)
{
    if (auto it = m_images.find(&bitmap); it != m_images.end()) {
        it->value.last_used_generation = m_generation;
        return it->value.image;
    }

    sk_sp<SkImage> image;
    auto* gr_context = m_skia_backend_context ? m_skia_backend_context->sk_context() : nullptr;
    if (gr_context && bitmap.yuv_data()) {
        auto const* yuv_data = bitmap.yuv_data();
        image = SkImages::TextureFromYUVAPixmaps(
            gr_context,
            yuv_data->make_pixmaps(),
            skgpu::Mipmapped::kNo,
            false,
            bitmap.color_space().color_space<sk_sp<SkColorSpace>>());
    }

    if (!image) {
        auto source_bitmap = bitmap.bitmap();
        if (!source_bitmap)
            return nullptr;

        auto raster_image = sk_image_from_bitmap(*source_bitmap, bitmap.color_space());
        if (gr_context) {
            image = SkImages::TextureFromImage(gr_context, raster_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
            if (!image)
                image = move(raster_image);
        } else {
            image = move(raster_image);
        }
    }

    if (!image)
        return nullptr;

    CachedImage cached_image {
        .bitmap = &bitmap,
        .image = image,
        .last_used_generation = m_generation,
    };
    m_images.set(&bitmap, move(cached_image));
    return image;
}

void ImmutableBitmapSkiaImageCache::prune()
{
    m_images.remove_all_matching([this](auto const&, auto const& cached_image) {
        return m_generation - cached_image.last_used_generation > image_cache_max_unused_generations;
    });
    ++m_generation;
}

}
