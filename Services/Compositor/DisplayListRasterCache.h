/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaCodecs/Forward.h>

class SkImage;
class SkImageFilter;
class SkPath;
class SkTextBlob;

template<typename T>
class sk_sp;

namespace Compositing {

struct ReplayLayer;

}

namespace Compositor {

struct TextBlobCacheKey {
    u64 font_id { 0 };
    u32 scale_bits { 0 };
    u8 font_smoothing { 0 };
    bool uses_lcd_text { false };
    u64 glyph_hash { 0 };

    bool operator==(TextBlobCacheKey const&) const = default;
};

}

namespace AK {

template<>
struct Traits<Compositor::TextBlobCacheKey> : public DefaultTraits<Compositor::TextBlobCacheKey> {
    static unsigned hash(Compositor::TextBlobCacheKey const& key)
    {
        auto font_options = (static_cast<unsigned>(key.font_smoothing) << 1) | key.uses_lcd_text;
        return pair_int_hash(pair_int_hash(u64_hash(key.font_id ^ key.glyph_hash), key.scale_bits), font_options);
    }
};

}

namespace Compositor {

// The Skia images and text blobs that the player makes from the fonts and the image frames that one display list, or
// what else is painted, draws. They are kept until it stops drawing what they were made from.
class DisplayListRasterCache {
    AK_MAKE_NONCOPYABLE(DisplayListRasterCache);
    AK_MAKE_NONMOVABLE(DisplayListRasterCache);

public:
    DisplayListRasterCache();
    ~DisplayListRasterCache();

    // Drops everything made from these fonts, which are no longer drawn.
    void forget_fonts(ReadonlySpan<u64> font_ids);
    // Drops everything made from these image frames, which are no longer drawn.
    void forget_image_frames(ReadonlySpan<u64> image_frame_ids);
    // Drops the image made from the frame of a video sink that is no longer drawn.
    void forget_video_sink(Media::VideoSinkHandle);
    // Drops the rasters of every SVG image but these, which are the ones that the display list of the context draws.
    void forget_vector_images_other_than(ReadonlySpan<u64> drawn_vector_image_ids);

    sk_sp<SkImage> image_for_image_frame(Gfx::DecodedImageFrame const&, Compositing::ImageFrameResourceId, RefPtr<Gfx::SkiaBackendContext> const&);
    // The image of the frame that the video sink shows. The image can be larger than the frame, whose visible size is
    // the part of it to draw.
    sk_sp<SkImage> image_for_video_sink(Media::VideoSinkHandle, Media::VideoFrame const&, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntSize& visible_size);
    sk_sp<SkImage> repeated_tile_raster(u64 tile_key, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&) const;
    void add_repeated_tile_raster(u64 tile_key, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&, sk_sp<SkImage>);
    // The rasters of an SVG image are made at a raster scale, a power of two of the size its records were recorded at.
    // Their rects are in the pixels of the image at that scale.
    sk_sp<SkImage> vector_image_raster(Compositing::DisplayListResourceId, int raster_scale, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntRect visible_rect_in_raster_pixels, Gfx::IntRect& raster_rect_in_raster_pixels);
    bool has_rasterized_vector_image(Compositing::DisplayListResourceId, int raster_scale) const;
    // Makes room for a raster of the image, which then counts against the budget until it is finished, and answers false
    // where the rasters in use leave no room for it.
    bool reserve_vector_image_raster(Compositing::DisplayListResourceId, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntRect rect_in_raster_pixels);
    // Keeps the raster that was reserved, unless it could not be made.
    sk_sp<SkImage> finish_vector_image_raster(Compositing::DisplayListResourceId, int raster_scale, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntRect rect_in_raster_pixels, sk_sp<SkImage>);
    sk_sp<SkTextBlob> text_blob(Gfx::Font const&, Compositing::FontResourceId, float scale, ReadonlySpan<Compositing::DisplayListGlyph>, u8 font_smoothing, bool uses_lcd_text = false);

    sk_sp<SkImageFilter> layer_image_filter(Compositing::ReplayLayer const&, Function<sk_sp<SkImageFilter>()> const& create);
    sk_sp<SkImageFilter> backdrop_image_filter(Compositing::ReplayLayer const&, Optional<Gfx::FloatRect> blur_edge, bool limited_to_region, Function<sk_sp<SkImageFilter>()> const& create);
    // Drops the filters of every layer but these, which are the ones whose effects the visual context tree of the
    // context holds.
    void keep_only_layer_filters(ReadonlySpan<u64> drawn_layer_filter_cache_keys);

    // The Skia path for a path that stays the same for as long as it has this identity. Keeping one Skia path lets
    // the caches that are keyed on it work.
    SkPath clip_path(u64 path_identity, Gfx::PathData const&);
    // Drops the Skia paths of every path but these, which are the ones that the clips of the visual context tree of
    // the context clip to.
    void keep_only_clip_paths(ReadonlySpan<u64> drawn_path_identities);

private:
    struct ImageFrameImage;
    struct VideoSinkImage;
    struct RepeatedTileRaster;
    struct VectorImageRasters;
    struct TextBlob;
    struct ClipPath;
    struct LayerImageFilterCache;
    LayerImageFilterCache& ensure_layer_image_filter_cache();

    OwnPtr<LayerImageFilterCache> m_layer_image_filter_cache;

    HashMap<u64, NonnullOwnPtr<ImageFrameImage>> m_image_frame_images;
    HashMap<u64, NonnullOwnPtr<VideoSinkImage>> m_video_sink_images;
    HashMap<u64, NonnullOwnPtr<RepeatedTileRaster>> m_repeated_tile_rasters;
    size_t m_repeated_tile_raster_bytes { 0 };
    HashMap<u64, NonnullOwnPtr<VectorImageRasters>> m_vector_image_rasters;
    size_t m_vector_image_raster_cache_reserved_bytes { 0 };
    HashMap<TextBlobCacheKey, NonnullOwnPtr<TextBlob>> m_text_blobs;
    size_t m_text_blob_cache_bytes { 0 };
    MonotonicTime m_text_blob_cache_sweep_time { MonotonicTime::now() };
    HashMap<u64, NonnullOwnPtr<ClipPath>> m_clip_paths;
};

}
