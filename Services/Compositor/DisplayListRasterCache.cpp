/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/ByteBuffer.h>
#include <Compositor/DisplayListRasterCache.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/VideoSurfaceImage.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFrameHandle.h>
#include <LibMedia/VideoSurface.h>

#include <core/SkFont.h>
#include <core/SkImage.h>
#include <core/SkTextBlob.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Compositor {

using namespace Compositing;

struct DisplayListRasterCache::ImageFrameImage {
    AK_ALLOC_WITH_KMALLOC;

    ImageFrameImage(NonnullRefPtr<Gfx::Bitmap const> bitmap, RefPtr<Gfx::SkiaBackendContext> skia_backend_context, sk_sp<SkImage> image)
        : bitmap(move(bitmap))
        , skia_backend_context(move(skia_backend_context))
        , image(move(image))
    {
    }

    // A raster image reads the pixels of the bitmap, so the bitmap must live as long as the image.
    NonnullRefPtr<Gfx::Bitmap const> bitmap;
    RefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    sk_sp<SkImage> image;
};

struct DisplayListRasterCache::VideoSinkImage {
    AK_ALLOC_WITH_KMALLOC;

    Media::VideoFramePoolID pool_id { 0 };
    u32 slot_index { 0 };
    u64 slot_acquisition_id { 0 };
    // A surface-backed image samples the surface where it lies, so it stays valid for every frame decoded into
    // that surface rather than only for the one acquisition.
    u32 surface_id { 0 };
    // Drawing is recorded here but read by the GPU afterwards, so the surface stays in use until a different one
    // is drawn in its place.
    Media::VideoSurfaceUse surface_use;
    RefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    sk_sp<SkImage> image;
};

struct DisplayListRasterCache::RepeatedTileRaster {
    AK_ALLOC_WITH_KMALLOC;

    RepeatedTileRaster(Gfx::IntSize tile_size, RefPtr<Gfx::SkiaBackendContext> skia_backend_context, sk_sp<SkImage> image)
        : tile_size(tile_size)
        , skia_backend_context(move(skia_backend_context))
        , image(move(image))
    {
    }

    size_t byte_size() const { return static_cast<size_t>(tile_size.width()) * tile_size.height() * 4; }

    Gfx::IntSize tile_size;
    RefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    sk_sp<SkImage> image;
};

struct DisplayListRasterCache::NestedDisplayListRasters {
    AK_ALLOC_WITH_KMALLOC;

    explicit NestedDisplayListRasters(RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
        : skia_backend_context(move(skia_backend_context))
    {
    }

    struct Raster {
        Gfx::IntRect rect_in_list_space;
        sk_sp<SkImage> image;
        MonotonicTime last_used { MonotonicTime::now() };

        size_t byte_size() const { return static_cast<size_t>(rect_in_list_space.width()) * rect_in_list_space.height() * 4; }
    };

    size_t total_byte_size() const
    {
        size_t total = 0;
        for (auto const& raster : rasters)
            total += raster.byte_size();
        return total;
    }

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    bool was_painted { false };

    // The same display list can be painted at several places in one frame (repeated SVG images, atlas-style
    // lists painted in many small slices), each with its own visible sub-rectangle, so a single list keeps a
    // bounded set of rasters, most recently used first.
    static constexpr size_t max_rasters = 32;
    Vector<Raster> rasters;
};

struct DisplayListRasterCache::TextBlob {
    AK_ALLOC_WITH_KMALLOC;

    TextBlob(ByteBuffer glyph_bytes, sk_sp<SkTextBlob> blob, size_t byte_size, MonotonicTime last_used)
        : glyph_bytes(move(glyph_bytes))
        , blob(move(blob))
        , byte_size(byte_size)
        , last_used(last_used)
    {
    }

    ByteBuffer glyph_bytes;
    sk_sp<SkTextBlob> blob;
    size_t byte_size { 0 };
    MonotonicTime last_used;
};

DisplayListRasterCache::DisplayListRasterCache() = default;
DisplayListRasterCache::~DisplayListRasterCache() = default;

void DisplayListRasterCache::evict(DisplayListResourceSet const& removed_resources)
{
    for (auto id : removed_resources.image_frames)
        m_image_frame_images.remove(id.value());
    for (auto id : removed_resources.video_sinks)
        m_video_sink_images.remove(id.value());
    for (auto id : removed_resources.display_lists)
        m_nested_display_list_rasters.remove(id.value());
    if (!removed_resources.fonts.is_empty()) {
        m_text_blobs.remove_all_matching([&](auto const& key, auto const& text_blob) {
            if (!removed_resources.fonts.contains(FontResourceId { key.font_id }))
                return false;
            m_text_blob_cache_bytes -= text_blob->byte_size;
            return true;
        });
    }
}

static sk_sp<SkImage> create_skia_image(Gfx::DecodedImageFrame const& frame, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto raster_image = Gfx::sk_image_from_bitmap(frame.bitmap(), frame.color_space());
    auto* gr_context = skia_backend_context ? skia_backend_context->sk_context() : nullptr;
    if (!gr_context)
        return raster_image;

    auto texture_image = SkImages::TextureFromImage(gr_context, raster_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
    if (texture_image)
        return texture_image;
    return raster_image;
}

sk_sp<SkImage> DisplayListRasterCache::image_for_image_frame(DisplayListResourceStorage const& resource_storage, ImageFrameResourceId id, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto const& frame = resource_storage.image_frame(id);
    if (auto cached = m_image_frame_images.get(id.value()); cached.has_value()) {
        auto const& image = *cached.value();
        // A frame replaced under the same id has a bitmap of its own, so this also catches a replacement that was
        // not evicted.
        if (image.bitmap.ptr() == &frame.bitmap() && image.skia_backend_context.ptr() == skia_backend_context.ptr())
            return image.image;
    }

    auto image = create_skia_image(frame, skia_backend_context);
    m_image_frame_images.set(id.value(), make<ImageFrameImage>(frame.bitmap_ref(), skia_backend_context, image));
    return image;
}

sk_sp<SkImage> DisplayListRasterCache::image_for_video_sink(DisplayListResourceStorage const& resource_storage, VideoSinkResourceId id, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto sink = resource_storage.video_sink(id);
    if (!sink)
        return nullptr;
    auto frame = sink->current_frame();
    if (!frame)
        return nullptr;

    auto handle = Media::VideoFrameHandle::for_frame(*frame);
    auto surface = frame->surface();
    auto surface_id = surface ? surface->id() : 0;
    auto& cached_image = *m_video_sink_images.ensure(id.value(), [] { return make<VideoSinkImage>(); });
    auto cached_image_matches = [&] {
        if (!cached_image.image)
            return false;
        if (cached_image.skia_backend_context != skia_backend_context)
            return false;
        if (surface_id != 0)
            return cached_image.surface_id == surface_id;
        if (cached_image.pool_id != handle.pool_id)
            return false;
        if (cached_image.slot_index != handle.slot_index)
            return false;
        if (cached_image.slot_acquisition_id != handle.slot_acquisition_id)
            return false;
        return true;
    }();
    if (cached_image_matches)
        return cached_image.image;

    sk_sp<SkImage> image;
    auto* gr_context = skia_backend_context ? skia_backend_context->sk_context() : nullptr;

#ifdef AK_OS_MACOS
    if (surface != nullptr && skia_backend_context)
        image = Gfx::sk_image_from_video_surface(surface->io_surface(), frame->cicp(), *skia_backend_context);
#endif

    auto yuv_data = image ? Optional<Gfx::YUVData> {} : frame->yuv_data();
    if (!image && !yuv_data.has_value() && surface == nullptr)
        return nullptr;

    auto color_space = Gfx::ColorSpace {};
    if (auto color_space_result = Gfx::ColorSpace::from_cicp(frame->cicp()); !color_space_result.is_error())
        color_space = color_space_result.release_value();

    if (!image && gr_context && yuv_data.has_value()) {
        image = SkImages::TextureFromYUVAPixmaps(
            gr_context,
            yuv_data->make_pixmaps(),
            skgpu::Mipmapped::kNo,
            false,
            Gfx::to_skia_color_space(color_space));
    }

    if (!image) {
        auto bitmap_or_error = [&] -> ErrorOr<NonnullRefPtr<Gfx::Bitmap>> {
#ifdef AK_OS_MACOS
            // Without a GPU to sample the surface on, its pixels have to be read back into memory to be drawn.
            if (surface != nullptr)
                return Gfx::bitmap_from_video_surface(surface->io_surface(), frame->cicp());
#endif
            return yuv_data->to_bitmap();
        }();
        if (bitmap_or_error.is_error()) {
            dbgln("Could not convert video frame to bitmap: {}", bitmap_or_error.release_error());
            return nullptr;
        }
        auto raster_image = Gfx::sk_image_adopting_bitmap(bitmap_or_error.release_value(), color_space);
        if (gr_context)
            image = SkImages::TextureFromImage(gr_context, raster_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
        if (!image)
            image = move(raster_image);
    }

    if (!frame->revalidate_backing())
        return nullptr;

    if (!image)
        return nullptr;

    cached_image.pool_id = handle.pool_id;
    cached_image.slot_index = handle.slot_index;
    cached_image.slot_acquisition_id = handle.slot_acquisition_id;
    cached_image.surface_id = surface_id;
    cached_image.surface_use = surface ? surface->begin_use() : Media::VideoSurfaceUse {};
    cached_image.skia_backend_context = skia_backend_context;
    cached_image.image = image;
    return image;
}

sk_sp<SkImage> DisplayListRasterCache::repeated_tile_raster(u64 tile_key, Gfx::IntSize tile_size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context) const
{
    auto cached_raster = m_repeated_tile_rasters.find(tile_key);
    if (cached_raster == m_repeated_tile_rasters.end())
        return nullptr;

    auto const& raster = *cached_raster->value;
    if (raster.tile_size != tile_size)
        return nullptr;
    if (raster.skia_backend_context.ptr() != skia_backend_context.ptr())
        return nullptr;

    return raster.image;
}

void DisplayListRasterCache::add_repeated_tile_raster(u64 tile_key, Gfx::IntSize tile_size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, sk_sp<SkImage> image)
{
    constexpr size_t max_repeated_tile_raster_bytes = 64 * MiB;
    auto raster = make<RepeatedTileRaster>(tile_size, skia_backend_context, move(image));
    if (m_repeated_tile_raster_bytes + raster->byte_size() > max_repeated_tile_raster_bytes) {
        m_repeated_tile_rasters.clear();
        m_repeated_tile_raster_bytes = 0;
    }
    if (auto existing = m_repeated_tile_rasters.get(tile_key); existing.has_value())
        m_repeated_tile_raster_bytes -= (*existing)->byte_size();
    m_repeated_tile_raster_bytes += raster->byte_size();
    m_repeated_tile_rasters.set(tile_key, move(raster));
}

sk_sp<SkImage> DisplayListRasterCache::nested_display_list_raster(DisplayListResourceId id, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, Gfx::IntRect visible_rect_in_list_space, Gfx::IntRect& raster_rect_in_list_space)
{
    auto cached_rasters = m_nested_display_list_rasters.find(id.value());
    if (cached_rasters == m_nested_display_list_rasters.end())
        return nullptr;

    auto& rasters = *cached_rasters->value;
    if (rasters.skia_backend_context.ptr() != skia_backend_context.ptr())
        return nullptr;

    for (size_t i = 0; i < rasters.rasters.size(); ++i) {
        if (!rasters.rasters[i].rect_in_list_space.contains(visible_rect_in_list_space))
            continue;
        if (i != 0) {
            auto raster = move(rasters.rasters[i]);
            rasters.rasters.remove(i);
            rasters.rasters.prepend(move(raster));
        }
        rasters.rasters.first().last_used = MonotonicTime::now();
        raster_rect_in_list_space = rasters.rasters.first().rect_in_list_space;
        return rasters.rasters.first().image;
    }
    return {};
}

void DisplayListRasterCache::add_nested_display_list_raster(DisplayListResourceId id, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context, Gfx::IntRect rect_in_list_space, sk_sp<SkImage> image)
{
    VERIFY(image);
    auto& rasters = *m_nested_display_list_rasters.ensure(id.value(), [&] {
        return make<NestedDisplayListRasters>(skia_backend_context);
    });

    if (rasters.skia_backend_context.ptr() != skia_backend_context.ptr()) {
        rasters.rasters.clear();
        rasters.skia_backend_context = skia_backend_context;
    }

    // When the budget is exceeded, evict rasters that are not part of the active working set (not used
    // recently). If everything is recently used, the live working set genuinely exceeds the budget; then skip
    // caching this raster instead of evicting hot entries, so an oversized page degrades to direct replay for
    // the overflow instead of thrashing the whole cache. Additions are rare by design, so the total is simply
    // recomputed here instead of being tracked incrementally.
    constexpr size_t max_nested_raster_cache_bytes = 512 * MiB;
    auto total_bytes = static_cast<size_t>(rect_in_list_space.width()) * rect_in_list_space.height() * 4;
    for (auto const& it : m_nested_display_list_rasters)
        total_bytes += it.value->total_byte_size();
    if (total_bytes > max_nested_raster_cache_bytes) {
        auto now = MonotonicTime::now();
        for (auto& it : m_nested_display_list_rasters) {
            it.value->rasters.remove_all_matching([&](auto const& raster) {
                if (now - raster.last_used < AK::Duration::from_milliseconds(250))
                    return false;
                total_bytes -= raster.byte_size();
                return true;
            });
        }
        if (total_bytes > max_nested_raster_cache_bytes)
            return;
    }

    if (rasters.rasters.size() == NestedDisplayListRasters::max_rasters)
        rasters.rasters.take_last();
    rasters.rasters.prepend({ rect_in_list_space, move(image) });
}

bool DisplayListRasterCache::should_cache_nested_display_list_raster(DisplayListResourceStorage const& resource_storage, DisplayListResourceId id)
{
    // Only rasterize a list that has been painted before: content that is re-recorded for every update gets a
    // fresh id each time and would waste a full rasterization per recording, while anything replayed more than
    // once converges to cache hits after its second paint.
    auto& rasters = *m_nested_display_list_rasters.ensure(id.value(), [&] {
        return make<NestedDisplayListRasters>(nullptr);
    });
    if (!exchange(rasters.was_painted, true))
        return false;
    return !resource_storage.display_list_requires_direct_replay(id);
}

static u64 text_blob_glyph_hash(ReadonlySpan<DisplayListGlyph> glyphs)
{
    u64 hash = glyphs.size();
    for (auto const& glyph : glyphs) {
        u64 position = static_cast<u64>(bit_cast<u32>(glyph.position.x())) << 32 | bit_cast<u32>(glyph.position.y());
        hash = (hash ^ position) * 0x9e3779b97f4a7c15ULL;
        hash = (hash ^ glyph.glyph_id) * 0x9e3779b97f4a7c15ULL;
        hash ^= hash >> 32;
    }
    return hash;
}

static sk_sp<SkTextBlob> make_text_blob(Gfx::Font const& font, float scale, ReadonlySpan<DisplayListGlyph> glyphs, [[maybe_unused]] u8 font_smoothing, TextRasterizationMode rasterization_mode)
{
    if (font.is_invisible())
        return nullptr;
    auto sk_font = font.skia_font(scale);
#ifdef AK_OS_MACOS
    // INTEROP: Blink disables CoreGraphics outline dilation for antialiased text.
    // https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/platform/fonts/mac/font_platform_data_mac.mm
    switch (static_cast<FontSmoothing>(font_smoothing)) {
    case FontSmoothing::Antialiased:
        sk_font.setEdging(SkFont::Edging::kAntiAlias);
        sk_font.setHinting(SkFontHinting::kNone);
        break;
    case FontSmoothing::None:
        sk_font.setEdging(SkFont::Edging::kAlias);
        break;
    case FontSmoothing::SubpixelAntialiased:
        sk_font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
        break;
    default:
        break;
    }
#endif
    if (rasterization_mode == TextRasterizationMode::Unhinted) {
        // NB: Preserve canvas text's fractional baseline and unhinted outline geometry.
        sk_font.setHinting(SkFontHinting::kNone);
        sk_font.setForceAutoHinting(false);
        sk_font.setBaselineSnap(false);
    }
    SkTextBlobBuilder builder;
    auto const& run = builder.allocRunPos(sk_font, glyphs.size());

    auto font_ascent = font.pixel_metrics().ascent;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        run.glyphs[i] = glyphs[i].glyph_id;
        run.pos[i * 2] = glyphs[i].position.x() * scale;
        run.pos[i * 2 + 1] = (glyphs[i].position.y() + font_ascent) * scale;
    }
    return builder.make();
}

sk_sp<SkTextBlob> DisplayListRasterCache::text_blob(DisplayListResourceStorage const& resource_storage, FontResourceId font_id, float scale, ReadonlySpan<DisplayListGlyph> glyphs, u8 font_smoothing, TextRasterizationMode rasterization_mode)
{
    constexpr size_t max_text_blob_cache_bytes = 16 * MiB;
    constexpr auto text_blob_idle_duration = AK::Duration::from_milliseconds(250);

    ReadonlyBytes glyph_bytes { reinterpret_cast<u8 const*>(glyphs.data()), glyphs.size() * sizeof(DisplayListGlyph) };
    TextBlobCacheKey key { font_id.value(), bit_cast<u32>(scale), font_smoothing, text_blob_glyph_hash(glyphs), rasterization_mode };
    auto now = MonotonicTime::now();

    if (auto cached = m_text_blobs.find(key); cached != m_text_blobs.end()) {
        auto& text_blob = *cached->value;
        if (text_blob.glyph_bytes.bytes() == glyph_bytes) {
            text_blob.last_used = now;
            return text_blob.blob;
        }
        m_text_blob_cache_bytes -= text_blob.byte_size;
        m_text_blobs.remove(cached);
    }

    auto blob = make_text_blob(resource_storage.font(font_id), scale, glyphs, font_smoothing, rasterization_mode);
    if (!blob)
        return nullptr;

    auto byte_size = glyphs.size() * 24 + 256;
    if (m_text_blob_cache_bytes + byte_size > max_text_blob_cache_bytes) {
        if (now - m_text_blob_cache_sweep_time < text_blob_idle_duration)
            return blob;
        m_text_blob_cache_sweep_time = now;
        m_text_blobs.remove_all_matching([&](auto const&, auto const& text_blob) {
            if (now - text_blob->last_used < text_blob_idle_duration)
                return false;
            m_text_blob_cache_bytes -= text_blob->byte_size;
            return true;
        });
        if (m_text_blob_cache_bytes + byte_size > max_text_blob_cache_bytes)
            return blob;
    }

    m_text_blob_cache_bytes += byte_size;
    m_text_blobs.set(key, make<TextBlob>(MUST(ByteBuffer::copy(glyph_bytes)), blob, byte_size, now));
    return blob;
}

}
