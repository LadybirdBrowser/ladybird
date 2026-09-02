/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>

class SkImage;
class SkTextBlob;

template<typename T>
class sk_sp;

namespace Web::Painting {

struct DisplayListResourceSet {
    bool is_empty() const;
    void include(DisplayListResourceSet const&);

    HashTable<FontResourceId> fonts;
    HashTable<ImageFrameResourceId> image_frames;
    HashTable<VideoSinkResourceId> video_sinks;
    HashTable<DisplayListResourceId> display_lists;
};

struct DisplayListFontResource {
    FontResourceId id;
    NonnullRefPtr<Gfx::Font const> font;
};

struct DisplayListImageFrameResource {
    ImageFrameResourceId id;
    Gfx::DecodedImageFrame frame;
};

struct DisplayListVideoSinkResource {
    VideoSinkResourceId id;
    // The display list routes a video by sink handle; the compositor resolves it to the hosted sink at
    // transaction-apply, and the draw reads the sink's current frame.
    Media::VideoSinkHandle sink_handle;
};

struct DisplayListTextBlobCacheKey {
    u64 font_id { 0 };
    u32 scale_bits { 0 };
    u64 glyph_hash { 0 };

    bool operator==(DisplayListTextBlobCacheKey const&) const = default;
};

struct DisplayListStoredImageFrameResource;
struct DisplayListCachedSkiaImageResource;
struct DisplayListCachedNestedRasterResource;
struct DisplayListCachedTextBlobResource;
struct DisplayListStoredVideoSinkResource;

struct DisplayListResource {
    DisplayListResource(NonnullRefPtr<DisplayList>, AccumulatedVisualContextTree);
    DisplayListResource(NonnullRefPtr<DisplayList const>, AccumulatedVisualContextTree);
    DisplayListResource(DisplayList const&, AccumulatedVisualContextTree);

    NonnullRefPtr<DisplayList const> display_list;
    AccumulatedVisualContextTree visual_context_tree;
};

struct DisplayListResourceTransaction {
    Vector<DisplayListFontResource> fonts;
    Vector<DisplayListImageFrameResource> image_frames;
    Vector<DisplayListVideoSinkResource> video_sinks;
    Vector<DisplayListResource> display_lists;

    Vector<FontResourceId> font_ids_to_remove;
    Vector<ImageFrameResourceId> image_frame_ids_to_remove;
    Vector<VideoSinkResourceId> video_sink_ids_to_remove;
    Vector<DisplayListResourceId> display_list_ids_to_remove;
};

class WEB_API DisplayListResourceStorage {
    AK_MAKE_NONCOPYABLE(DisplayListResourceStorage);

public:
    DisplayListResourceStorage();
    DisplayListResourceStorage(DisplayListResourceStorage&&);
    DisplayListResourceStorage& operator=(DisplayListResourceStorage&&);
    ~DisplayListResourceStorage();

    FontResourceId add_font(Gfx::Font const&);
    ImageFrameResourceId add_image_frame(Gfx::DecodedImageFrame const&);
    VideoSinkResourceId add_video_sink(VideoSinkResourceId, Media::VideoSinkHandle);
    DisplayListResourceId add_display_list(NonnullRefPtr<DisplayList const>, AccumulatedVisualContextTree const&);
    DisplayListResourceId add_display_list(DisplayListResource&&);
    bool has_display_list(DisplayListResourceId id) const { return m_display_lists.contains(id.value()); }
    void set_font(FontResourceId, NonnullRefPtr<Gfx::Font const>);
    void set_image_frame(ImageFrameResourceId, Gfx::DecodedImageFrame);
    void apply_transaction(DisplayListResourceTransaction&&);
    DisplayListResourceTransaction create_transaction(DisplayListResourceSet const& previous, DisplayListResourceSet const& current) const;
    DisplayListResourceSet collect_referenced_resources(DisplayList const&) const;
    DisplayListResourceSet collect_referenced_resources(DisplayList const&, AccumulatedVisualContextTree const&) const;
    DisplayListResourceSet collect_referenced_resources(AccumulatedVisualContextTree const&) const;
    DisplayListResourceSet collect_referenced_resources(ReadonlyBytes command_bytes) const;
    void retain_only(DisplayListResourceSet const&);
    bool has_resources_added_since_last_retain() const { return m_has_resources_added_since_last_retain; }
    void set_video_sink(VideoSinkResourceId, RefPtr<Media::VideoSink>);

    Gfx::Font const& font(FontResourceId id) const { return *m_fonts.get(id.value()).value(); }
    Gfx::DecodedImageFrame const& image_frame(ImageFrameResourceId) const;
    sk_sp<SkImage> skia_image_for_image_frame(ImageFrameResourceId, RefPtr<Gfx::SkiaBackendContext> const&) const;
    sk_sp<SkImage> skia_image_for_video_sink(VideoSinkResourceId, RefPtr<Gfx::SkiaBackendContext> const&) const;
    sk_sp<SkImage> cached_skia_image_for_display_list(DisplayListResourceId, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&) const;
    void set_cached_skia_image_for_display_list(DisplayListResourceId, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&, sk_sp<SkImage>) const;
    sk_sp<SkImage> cached_nested_display_list_raster(DisplayListResourceId, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntRect visible_rect_in_list_space, Gfx::IntRect& raster_rect_in_list_space) const;
    void add_cached_nested_display_list_raster(DisplayListResourceId, RefPtr<Gfx::SkiaBackendContext> const&, Gfx::IntRect rect_in_list_space, sk_sp<SkImage>) const;
    bool should_cache_nested_display_list_raster(DisplayListResourceId) const;
    sk_sp<SkTextBlob> text_blob(FontResourceId, float scale, ReadonlySpan<DisplayListGlyph>) const;
    RefPtr<Media::VideoSink const> video_sink(VideoSinkResourceId id) const;
    Optional<Media::VideoSinkHandle> video_sink_handle(VideoSinkResourceId id) const { return m_video_sink_handles.get(id.value()); }
    HashMap<u64, Media::VideoSinkHandle> const& video_sink_handles() const { return m_video_sink_handles; }
    DisplayListResource const& display_list_resource(DisplayListResourceId id) const { return m_display_lists.get(id.value()).value(); }
    DisplayList const& display_list(DisplayListResourceId id) const { return *display_list_resource(id).display_list; }
    AccumulatedVisualContextTree const& display_list_visual_context_tree(DisplayListResourceId id) const { return display_list_resource(id).visual_context_tree; }

private:
    void collect_referenced_resources(ReadonlyBytes command_bytes, DisplayListResourceSet&) const;
    void collect_referenced_resources(DisplayList const&, DisplayListResourceSet&) const;
    void collect_referenced_resources(AccumulatedVisualContextTree const&, DisplayListResourceSet&) const;
    void add_referenced_display_list(DisplayListResourceId, DisplayListResourceSet&) const;
    bool nested_display_list_requires_direct_replay(DisplayListResourceId, HashTable<u64>& visited_display_lists) const;
    void remove_text_blobs_without_font();

    bool m_has_resources_added_since_last_retain { false };
    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts;
    HashMap<u64, NonnullOwnPtr<DisplayListStoredImageFrameResource>> m_image_frames;
    HashMap<u64, Media::VideoSinkHandle> m_video_sink_handles;
    HashMap<u64, NonnullOwnPtr<DisplayListStoredVideoSinkResource>> m_video_sinks;
    HashMap<u64, DisplayListResource> m_display_lists;
    mutable HashMap<u64, NonnullOwnPtr<DisplayListCachedSkiaImageResource>> m_display_list_cached_skia_images;
    mutable HashMap<u64, NonnullOwnPtr<DisplayListCachedNestedRasterResource>> m_display_list_cached_nested_rasters;
    mutable HashMap<DisplayListTextBlobCacheKey, NonnullOwnPtr<DisplayListCachedTextBlobResource>> m_text_blobs;
    mutable size_t m_text_blob_cache_bytes { 0 };
    mutable MonotonicTime m_text_blob_cache_sweep_time { MonotonicTime::now() };
};

}

namespace AK {

template<>
struct Traits<Web::Painting::DisplayListTextBlobCacheKey> : public DefaultTraits<Web::Painting::DisplayListTextBlobCacheKey> {
    static unsigned hash(Web::Painting::DisplayListTextBlobCacheKey const& key)
    {
        return pair_int_hash(u64_hash(key.font_id ^ key.glyph_hash), key.scale_bits);
    }
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayListFontResource const&);
template<>
WEB_API ErrorOr<Web::Painting::DisplayListFontResource> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayListImageFrameResource const&);
template<>
WEB_API ErrorOr<Web::Painting::DisplayListImageFrameResource> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayListVideoSinkResource const&);
template<>
WEB_API ErrorOr<Web::Painting::DisplayListVideoSinkResource> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayListResourceTransaction const&);
template<>
WEB_API ErrorOr<Web::Painting::DisplayListResourceTransaction> decode(Decoder&);

}
