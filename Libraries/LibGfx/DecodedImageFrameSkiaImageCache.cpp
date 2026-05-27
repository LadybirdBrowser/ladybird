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
        u64 last_used_sequence_number { 0 };
        size_t approximate_byte_size { 0 };
    };

    u64 next_use_sequence_number()
    {
        return ++use_sequence_number;
    }

    void prune_to_limits()
    {
        while (images.size() > image_cache_max_entries || approximate_byte_size > image_cache_max_bytes) {
            Optional<DecodedImageFrame> least_recently_used_frame;
            Optional<u64> least_recently_used_sequence_number;
            for (auto const& image : images) {
                if (!least_recently_used_sequence_number.has_value()
                    || image.value.last_used_sequence_number < least_recently_used_sequence_number.value()) {
                    least_recently_used_frame = image.key;
                    least_recently_used_sequence_number = image.value.last_used_sequence_number;
                }
            }

            if (!least_recently_used_frame.has_value())
                break;

            auto cached_image = images.get(least_recently_used_frame.value()).release_value();
            approximate_byte_size -= min(approximate_byte_size, cached_image.approximate_byte_size);
            images.remove(least_recently_used_frame.value());
        }
    }

    RefPtr<SkiaBackendContext> skia_backend_context;
    HashMap<DecodedImageFrame, CachedImage, DecodedImageFrameKeyTraits> images;
    size_t approximate_byte_size { 0 };
    u64 use_sequence_number { 0 };
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
        it->value.last_used_sequence_number = m_impl->next_use_sequence_number();
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
        .last_used_sequence_number = m_impl->next_use_sequence_number(),
        .approximate_byte_size = bitmap.size_in_bytes(),
    };
    m_impl->approximate_byte_size += cached_image.approximate_byte_size;
    m_impl->images.set(frame, move(cached_image));
    m_impl->prune_to_limits();
    return image;
}

void DecodedImageFrameSkiaImageCache::prune()
{
    m_impl->prune_to_limits();
}

}
