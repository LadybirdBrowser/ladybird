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
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>

namespace Web::Painting {

struct DisplayListResourceSet {
    HashTable<FontResourceId> fonts;
    HashTable<ImageFrameResourceId> image_frames;
    HashTable<VideoFrameResourceId> video_frames;
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

struct DisplayListVideoFrameResource {
    VideoFrameResourceId id;
    RefPtr<Media::VideoFrame const> frame;
};

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
    Vector<DisplayListVideoFrameResource> video_frames;
    Vector<DisplayListResource> display_lists;

    Vector<FontResourceId> font_ids_to_remove;
    Vector<ImageFrameResourceId> image_frame_ids_to_remove;
    Vector<VideoFrameResourceId> video_frame_ids_to_remove;
    Vector<DisplayListResourceId> display_list_ids_to_remove;
};

class WEB_API DisplayListResourceStorage {
    AK_MAKE_NONCOPYABLE(DisplayListResourceStorage);
    AK_MAKE_DEFAULT_MOVABLE(DisplayListResourceStorage);

public:
    DisplayListResourceStorage() = default;
    ~DisplayListResourceStorage();

    FontResourceId add_font(Gfx::Font const&);
    ImageFrameResourceId add_image_frame(Gfx::DecodedImageFrame const&);
    VideoFrameResourceId add_video_frame(VideoFrameResourceId, RefPtr<Media::VideoFrame const> = nullptr);
    DisplayListResourceId add_display_list(NonnullRefPtr<DisplayList const>, AccumulatedVisualContextTree const&);
    DisplayListResourceId add_display_list(DisplayListResource&&);
    void set_font(FontResourceId, NonnullRefPtr<Gfx::Font const>);
    void set_image_frame(ImageFrameResourceId, Gfx::DecodedImageFrame);
    void append_referenced_resources_from(DisplayListResourceStorage const& source, ReadonlyBytes command_bytes);
    void apply_transaction(DisplayListResourceTransaction&&);
    DisplayListResourceTransaction create_transaction(DisplayListResourceSet const& previous, DisplayListResourceSet const& current) const;
    DisplayListResourceSet collect_referenced_resources(DisplayList const&) const;
    DisplayListResourceSet collect_referenced_resources(ReadonlyBytes command_bytes) const;
    void retain_only(DisplayListResourceSet const&);
    void update_video_frame(VideoFrameResourceId, NonnullRefPtr<Media::VideoFrame const>);
    void clear_video_frame(VideoFrameResourceId);
    void update_compositor_surface(CompositorSurfaceId, Gfx::SharedImage&&);
    void clear_compositor_surface(CompositorSurfaceId);

    Gfx::Font const& font(FontResourceId id) const { return *m_fonts.get(id.value()).value(); }
    Gfx::DecodedImageFrame const& image_frame(ImageFrameResourceId id) const { return m_image_frames.get(id.value()).value(); }
    RefPtr<Media::VideoFrame const> video_frame(VideoFrameResourceId id) const { return m_video_frames.get(id.value()).value(); }
    DisplayListResource const& display_list_resource(DisplayListResourceId id) const { return m_display_lists.get(id.value()).value(); }
    DisplayList const& display_list(DisplayListResourceId id) const { return *display_list_resource(id).display_list; }
    AccumulatedVisualContextTree const& display_list_visual_context_tree(DisplayListResourceId id) const { return display_list_resource(id).visual_context_tree; }
    Optional<Gfx::DecodedImageFrame const&> compositor_surface(CompositorSurfaceId id) const { return m_compositor_surfaces.get(id.value()); }

private:
    void collect_referenced_resources(ReadonlyBytes command_bytes, DisplayListResourceSet&) const;

    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts;
    HashMap<u64, Gfx::DecodedImageFrame> m_image_frames;
    HashMap<u64, RefPtr<Media::VideoFrame const>> m_video_frames;
    HashMap<u64, DisplayListResource> m_display_lists;
    HashMap<u64, Gfx::DecodedImageFrame> m_compositor_surfaces;
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
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayListVideoFrameResource const&);
template<>
WEB_API ErrorOr<Web::Painting::DisplayListVideoFrameResource> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::DisplayListResourceTransaction const&);
template<>
WEB_API ErrorOr<Web::Painting::DisplayListResourceTransaction> decode(Decoder&);

}
