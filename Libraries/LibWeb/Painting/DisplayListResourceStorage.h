/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/VideoFrameSource.h>

namespace Web::Painting {

class DisplayListResourceStorage : public AtomicRefCounted<DisplayListResourceStorage> {
public:
    static NonnullRefPtr<DisplayListResourceStorage> create()
    {
        return adopt_ref(*new DisplayListResourceStorage);
    }

    ~DisplayListResourceStorage();

    FontResourceId add_font(Gfx::Font const&);
    ImageFrameResourceId add_image_frame(Gfx::DecodedImageFrame const&);
    ExternalContentResourceId add_external_content_source(NonnullRefPtr<ExternalContentSource const>);
    VideoFrameResourceId add_video_frame_source(NonnullRefPtr<VideoFrameSource const>);
    FilterResourceId add_filter(Gfx::Filter const&);
    PaintStyleResourceId add_paint_style(RefPtr<SVGPaintServerPaintStyle const>);
    DisplayListResourceId add_display_list(NonnullRefPtr<DisplayList const>);
    void append_referenced_resources_from(DisplayListResourceStorage const& source, ReadonlySpan<DisplayListCommand> commands);

    Gfx::Font const& font(FontResourceId id) const { return *m_fonts.get(id.value()).value(); }
    Gfx::DecodedImageFrame const& image_frame(ImageFrameResourceId id) const { return m_image_frames.get(id.value()).value(); }
    ExternalContentSource const& external_content_source(ExternalContentResourceId id) const { return *m_external_content_sources.get(id.value()).value(); }
    VideoFrameSource const& video_frame_source(VideoFrameResourceId id) const { return *m_video_frame_sources.get(id.value()).value(); }
    Gfx::Filter const& filter(FilterResourceId id) const { return m_filters.get(id.value()).value(); }
    SVGPaintServerPaintStyle const& paint_style(PaintStyleResourceId id) const { return *m_paint_styles.get(id.value()).value(); }
    DisplayList const& display_list(DisplayListResourceId id) const { return *m_display_lists.get(id.value()).value(); }

private:
    DisplayListResourceStorage() = default;

    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts;
    HashMap<u64, Gfx::DecodedImageFrame> m_image_frames;
    HashMap<u64, NonnullRefPtr<ExternalContentSource const>> m_external_content_sources;
    HashMap<u64, NonnullRefPtr<VideoFrameSource const>> m_video_frame_sources;
    HashMap<u64, Gfx::Filter> m_filters;
    HashMap<u64, RefPtr<SVGPaintServerPaintStyle const>> m_paint_styles;
    HashMap<u64, NonnullRefPtr<DisplayList const>> m_display_lists;
};

}
