/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/Painting/VideoFrameSource.h>

namespace Web::Painting {

struct DisplayListResourceSet {
    HashTable<FontResourceId> fonts;
    HashTable<ImageFrameResourceId> image_frames;
    HashTable<ExternalContentResourceId> external_content_sources;
    HashTable<VideoFrameResourceId> video_frame_sources;
    HashTable<DisplayListResourceId> display_lists;
};

struct DisplayListResourceTransaction {
    Vector<NonnullRefPtr<Gfx::Font const>> fonts;
    Vector<Gfx::DecodedImageFrame> image_frames;
    Vector<NonnullRefPtr<ExternalContentSource const>> external_content_sources;
    Vector<NonnullRefPtr<VideoFrameSource const>> video_frame_sources;
    Vector<NonnullRefPtr<DisplayList const>> display_lists;

    Vector<FontResourceId> font_ids_to_remove;
    Vector<ImageFrameResourceId> image_frame_ids_to_remove;
    Vector<ExternalContentResourceId> external_content_source_ids_to_remove;
    Vector<VideoFrameResourceId> video_frame_source_ids_to_remove;
    Vector<DisplayListResourceId> display_list_ids_to_remove;
};

class DisplayListResourceStorage {
    AK_MAKE_NONCOPYABLE(DisplayListResourceStorage);
    AK_MAKE_DEFAULT_MOVABLE(DisplayListResourceStorage);

public:
    DisplayListResourceStorage() = default;
    ~DisplayListResourceStorage();

    FontResourceId add_font(Gfx::Font const&);
    ImageFrameResourceId add_image_frame(Gfx::DecodedImageFrame const&);
    ExternalContentResourceId add_external_content_source(NonnullRefPtr<ExternalContentSource const>);
    VideoFrameResourceId add_video_frame_source(NonnullRefPtr<VideoFrameSource const>);
    DisplayListResourceId add_display_list(NonnullRefPtr<DisplayList const>);
    void append_referenced_resources_from(DisplayListResourceStorage const& source, ReadonlyBytes command_bytes);
    void apply_transaction(DisplayListResourceTransaction&&);
    DisplayListResourceTransaction create_transaction(DisplayListResourceSet const& previous, DisplayListResourceSet const& current) const;
    DisplayListResourceSet collect_referenced_resources(DisplayList const&) const;
    DisplayListResourceSet collect_referenced_resources(ReadonlyBytes command_bytes) const;
    void retain_only(DisplayListResourceSet const&);

    Gfx::Font const& font(FontResourceId id) const { return *m_fonts.get(id.value()).value(); }
    Gfx::DecodedImageFrame const& image_frame(ImageFrameResourceId id) const { return m_image_frames.get(id.value()).value(); }
    ExternalContentSource const& external_content_source(ExternalContentResourceId id) const { return *m_external_content_sources.get(id.value()).value(); }
    VideoFrameSource const& video_frame_source(VideoFrameResourceId id) const { return *m_video_frame_sources.get(id.value()).value(); }
    DisplayList const& display_list(DisplayListResourceId id) const { return *m_display_lists.get(id.value()).value(); }

private:
    void collect_referenced_resources(ReadonlyBytes command_bytes, DisplayListResourceSet&) const;

    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts;
    HashMap<u64, Gfx::DecodedImageFrame> m_image_frames;
    HashMap<u64, NonnullRefPtr<ExternalContentSource const>> m_external_content_sources;
    HashMap<u64, NonnullRefPtr<VideoFrameSource const>> m_video_frame_sources;
    HashMap<u64, NonnullRefPtr<DisplayList const>> m_display_lists;
};

}
