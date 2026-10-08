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
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoSinkHandle.h>

namespace Compositing {

struct COMPOSITING_API DisplayListResourceSet {
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

// The -webkit-font-smoothing values the recorder writes as a byte, in the order the CSS enum declares them.
enum class FontSmoothing : u8 {
    Auto,
    None,
    Antialiased,
    SubpixelAntialiased,
};

struct DisplayListStoredImageFrameResource;
struct DisplayListStoredVideoSinkResource;

struct COMPOSITING_API DisplayListResource {
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

class COMPOSITING_API DisplayListResourceStorage {
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
    // Returns the resources that the transaction removed or replaced, so whatever was made from them can be dropped.
    DisplayListResourceSet apply_transaction(DisplayListResourceTransaction&&);
    DisplayListResourceTransaction create_transaction(DisplayListResourceSet const& previous, DisplayListResourceSet const& current) const;
    DisplayListResourceSet collect_referenced_resources(DisplayList const&) const;
    DisplayListResourceSet collect_referenced_resources(AccumulatedVisualContextTree const&) const;
    // Carries everything the list and its tree refer to, for a receiver that holds none of it yet.
    ErrorOr<DisplayListResourceTransaction> create_self_contained_transaction(DisplayList const&, AccumulatedVisualContextTree const&) const;
    // Checks that the list, and each list nested in it, can replay against what this storage holds, and that no nested
    // list reaches itself.
    ErrorOr<void> validate_for_replay(DisplayList const&, AccumulatedVisualContextTree const&) const;
    // Returns the resources that were removed.
    DisplayListResourceSet retain_only(DisplayListResourceSet const&);
    bool has_resources_added_since_last_retain() const { return m_has_resources_added_since_last_retain; }
    void set_video_sink(VideoSinkResourceId, RefPtr<Media::VideoSink>);

    bool has_font(FontResourceId id) const { return m_fonts.contains(id.value()); }
    bool has_image_frame(ImageFrameResourceId id) const { return m_image_frames.contains(id.value()); }
    Gfx::Font const& font(FontResourceId id) const { return *m_fonts.get(id.value()).value(); }
    Gfx::DecodedImageFrame const& image_frame(ImageFrameResourceId) const;
    // Whether force-dark should invert this image, worked out once and cached.
    bool image_frame_should_force_dark(ImageFrameResourceId) const;
    // Whether reusing a raster of the display list could show other pixels than a replay in place, worked out once
    // and cached.
    bool display_list_requires_direct_replay(DisplayListResourceId) const;
    RefPtr<Media::VideoSink const> video_sink(VideoSinkResourceId id) const;
    Optional<Media::VideoSinkHandle> video_sink_handle(VideoSinkResourceId id) const { return m_video_sink_handles.get(id.value()); }
    HashMap<u64, Media::VideoSinkHandle> const& video_sink_handles() const { return m_video_sink_handles; }
    DisplayListResource const& display_list_resource(DisplayListResourceId id) const { return m_display_lists.get(id.value()).value(); }
    DisplayList const& display_list(DisplayListResourceId id) const { return *display_list_resource(id).display_list; }
    AccumulatedVisualContextTree const& display_list_visual_context_tree(DisplayListResourceId id) const { return display_list_resource(id).visual_context_tree; }

private:
    void set_image_frame(ImageFrameResourceId, Gfx::DecodedImageFrame);
    void collect_referenced_resources(ReadonlyBytes command_bytes, DisplayListResourceSet&) const;
    void collect_referenced_resources(DisplayList const&, DisplayListResourceSet&) const;
    void collect_referenced_resources(AccumulatedVisualContextTree const&, DisplayListResourceSet&) const;
    void add_referenced_display_list(DisplayListResourceId, DisplayListResourceSet&) const;
    bool nested_display_list_requires_direct_replay(DisplayListResourceId, HashTable<u64>& visited_display_lists) const;

    bool m_has_resources_added_since_last_retain { false };
    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts;
    HashMap<u64, NonnullOwnPtr<DisplayListStoredImageFrameResource>> m_image_frames;
    HashMap<u64, Media::VideoSinkHandle> m_video_sink_handles;
    HashMap<u64, NonnullOwnPtr<DisplayListStoredVideoSinkResource>> m_video_sinks;
    HashMap<u64, DisplayListResource> m_display_lists;
    mutable HashMap<u64, bool> m_display_list_requires_direct_replay;
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListFontResource const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListFontResource> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListImageFrameResource const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListImageFrameResource> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListVideoSinkResource const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListVideoSinkResource> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListResourceTransaction const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListResourceTransaction> decode(Decoder&);

}
