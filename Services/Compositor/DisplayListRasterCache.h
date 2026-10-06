/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

class SkImage;
class SkTextBlob;

template<typename T>
class sk_sp;

namespace Compositor {

enum class TextRasterizationMode : u8 {
    Normal,
    Unhinted,
};

struct TextBlobCacheKey {
    u64 font_id { 0 };
    u32 scale_bits { 0 };
    u8 font_smoothing { 0 };
    u64 glyph_hash { 0 };
    TextRasterizationMode rasterization_mode { TextRasterizationMode::Normal };

    bool operator==(TextBlobCacheKey const&) const = default;
};

}

namespace AK {

template<>
struct Traits<Compositor::TextBlobCacheKey> : public DefaultTraits<Compositor::TextBlobCacheKey> {
    static unsigned hash(Compositor::TextBlobCacheKey const& key)
    {
        return pair_int_hash(pair_int_hash(u64_hash(key.font_id ^ key.glyph_hash), key.scale_bits), pair_int_hash(key.font_smoothing, static_cast<u8>(key.rasterization_mode)));
    }
};

}

namespace Compositor {

// The Skia images and text blobs that the player makes from the resources of one storage. They are kept until the
// storage removes the resource they were made from.
class DisplayListRasterCache {
    AK_MAKE_NONCOPYABLE(DisplayListRasterCache);
    AK_MAKE_NONMOVABLE(DisplayListRasterCache);

public:
    DisplayListRasterCache();
    ~DisplayListRasterCache();

    // Drops everything made from these resources, which the storage removed or replaced.
    void evict(Compositing::DisplayListResourceSet const& removed_resources);

    sk_sp<SkImage> image_for_image_frame(Compositing::DisplayListResourceStorage const&, Compositing::ImageFrameResourceId, RefPtr<Gfx::SkiaBackendContext> const&);
    sk_sp<SkImage> image_for_video_sink(Compositing::DisplayListResourceStorage const&, Compositing::VideoSinkResourceId, RefPtr<Gfx::SkiaBackendContext> const&);
    sk_sp<SkImage> repeated_tile_raster(u64 tile_key, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&) const;
    void add_repeated_tile_raster(u64 tile_key, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&, sk_sp<SkImage>);
    sk_sp<SkImage> nested_display_list_raster(Compositing::DisplayListResourceId, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntRect visible_rect_in_list_space, Gfx::IntRect& raster_rect_in_list_space);
    void add_nested_display_list_raster(Compositing::DisplayListResourceId, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntRect rect_in_list_space, sk_sp<SkImage>);
    bool should_cache_nested_display_list_raster(Compositing::DisplayListResourceStorage const&, Compositing::DisplayListResourceId);
    sk_sp<SkTextBlob> text_blob(Compositing::DisplayListResourceStorage const&, Compositing::FontResourceId, float scale, ReadonlySpan<Compositing::DisplayListGlyph>, u8 font_smoothing, TextRasterizationMode = TextRasterizationMode::Normal);

private:
    struct ImageFrameImage;
    struct VideoSinkImage;
    struct RepeatedTileRaster;
    struct NestedDisplayListRasters;
    struct TextBlob;

    HashMap<u64, NonnullOwnPtr<ImageFrameImage>> m_image_frame_images;
    HashMap<u64, NonnullOwnPtr<VideoSinkImage>> m_video_sink_images;
    HashMap<u64, NonnullOwnPtr<RepeatedTileRaster>> m_repeated_tile_rasters;
    size_t m_repeated_tile_raster_bytes { 0 };
    HashMap<u64, NonnullOwnPtr<NestedDisplayListRasters>> m_nested_display_list_rasters;
    HashMap<TextBlobCacheKey, NonnullOwnPtr<TextBlob>> m_text_blobs;
    size_t m_text_blob_cache_bytes { 0 };
    MonotonicTime m_text_blob_cache_sweep_time { MonotonicTime::now() };
};

}
