/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/ImmutableBitmapSkiaImageCache.h>
#include <LibGfx/SkiaBackendContext.h>

#include <core/SkImage.h>

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

    auto context = m_skia_backend_context ? m_skia_backend_context : SkiaBackendContext::the();
    if (context && !bitmap.ensure_sk_image(*context))
        return nullptr;

    auto image = sk_ref_sp(bitmap.sk_image());
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
