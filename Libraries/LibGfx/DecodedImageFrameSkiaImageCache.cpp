/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/DecodedImageFrameSkiaImageCache.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkRefCnt.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Gfx {

static constexpr u64 image_cache_max_unused_generations = 120;
static constexpr size_t image_cache_max_entries = 128;
static constexpr size_t image_cache_max_bytes = 64 * MiB;

struct DecodedImageFrameSkiaImageCache::Impl {
    Impl() = default;

    explicit Impl(RefPtr<SkiaBackendContext> skia_backend_context)
        : skia_backend_context(move(skia_backend_context))
    {
    }

    struct DecodedImageFrameKeyTraits : public Traits<DecodedImageFrame> {
        static unsigned hash(DecodedImageFrame const& frame)
        {
            return pair_int_hash(
                ptr_hash(&frame.bitmap()),
                ptr_hash(color_space_pointer(frame)));
        }

        static bool equals(DecodedImageFrame const& a, DecodedImageFrame const& b)
        {
            return &a.bitmap() == &b.bitmap()
                && color_space_pointer(a) == color_space_pointer(b);
        }

        static constexpr bool may_have_slow_equality_check() { return false; }

    private:
        static SkColorSpace const* color_space_pointer(DecodedImageFrame const& frame)
        {
            return frame.color_space().color_space<sk_sp<SkColorSpace>>().get();
        }
    };

    struct CachedImage {
        sk_sp<SkImage> image;
        u64 last_used_generation { 0 };
        size_t approximate_byte_size { 0 };
    };

    void prune_to_limits()
    {
        while (images.size() > image_cache_max_entries || approximate_byte_size > image_cache_max_bytes) {
            Optional<DecodedImageFrame> oldest_frame;
            Optional<u64> oldest_generation;
            for (auto const& image : images) {
                if (!oldest_generation.has_value() || image.value.last_used_generation < oldest_generation.value()) {
                    oldest_frame = image.key;
                    oldest_generation = image.value.last_used_generation;
                }
            }

            if (!oldest_frame.has_value())
                break;

            auto cached_image = images.get(oldest_frame.value()).release_value();
            approximate_byte_size -= min(approximate_byte_size, cached_image.approximate_byte_size);
            images.remove(oldest_frame.value());
        }
    }

    RefPtr<SkiaBackendContext> skia_backend_context;
    HashMap<DecodedImageFrame, CachedImage, DecodedImageFrameKeyTraits> images;
    size_t approximate_byte_size { 0 };
    u64 generation { 0 };
};

DecodedImageFrameSkiaImageCache::DecodedImageFrameSkiaImageCache()
    : m_impl(make<Impl>())
{
}

DecodedImageFrameSkiaImageCache::DecodedImageFrameSkiaImageCache(RefPtr<SkiaBackendContext> skia_backend_context)
    : m_impl(make<Impl>(move(skia_backend_context)))
{
}

DecodedImageFrameSkiaImageCache::~DecodedImageFrameSkiaImageCache() = default;

sk_sp<SkImage> DecodedImageFrameSkiaImageCache::image_for_frame(DecodedImageFrame const& frame)
{
    auto const& bitmap = frame.bitmap();
    if (auto it = m_impl->images.find(frame); it != m_impl->images.end()) {
        it->value.last_used_generation = m_impl->generation;
        return it->value.image;
    }

    auto raster_image = sk_image_from_bitmap(bitmap, frame.color_space());
    sk_sp<SkImage> image;
    auto* gr_context = m_impl->skia_backend_context ? m_impl->skia_backend_context->sk_context() : nullptr;
    if (gr_context) {
        image = SkImages::TextureFromImage(gr_context, raster_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
        if (!image)
            image = move(raster_image);
    } else {
        image = move(raster_image);
    }

    if (!image)
        return nullptr;

    Impl::CachedImage cached_image {
        .image = image,
        .last_used_generation = m_impl->generation,
        .approximate_byte_size = bitmap.size_in_bytes(),
    };
    m_impl->approximate_byte_size += cached_image.approximate_byte_size;
    m_impl->images.set(frame, move(cached_image));
    m_impl->prune_to_limits();
    return image;
}

void DecodedImageFrameSkiaImageCache::prune()
{
    m_impl->images.remove_all_matching([this](auto const&, auto const& cached_image) {
        if (m_impl->generation - cached_image.last_used_generation <= image_cache_max_unused_generations)
            return false;
        m_impl->approximate_byte_size -= min(m_impl->approximate_byte_size, cached_image.approximate_byte_size);
        return true;
    });
    m_impl->prune_to_limits();
    ++m_impl->generation;
}

}
