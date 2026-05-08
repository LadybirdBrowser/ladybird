/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::Painting {

DisplayListResourceStorage::~DisplayListResourceStorage() = default;

FontResourceId DisplayListResourceStorage::add_font(Gfx::Font const& font)
{
    auto id = font.id();
    m_fonts.ensure(id, [&]() -> NonnullRefPtr<Gfx::Font const> {
        return font;
    });
    return { id };
}

ImageFrameResourceId DisplayListResourceStorage::add_image_frame(Gfx::DecodedImageFrame const& frame)
{
    auto id = frame.id();
    m_image_frames.ensure(id, [&] {
        return frame;
    });
    return { id };
}

ExternalContentResourceId DisplayListResourceStorage::add_external_content_source(
    NonnullRefPtr<ExternalContentSource const> source)
{
    auto id = source->id();
    m_external_content_sources.ensure(id, [&] {
        return move(source);
    });
    return { id };
}

VideoFrameResourceId DisplayListResourceStorage::add_video_frame_source(NonnullRefPtr<VideoFrameSource const> source)
{
    auto id = source->id();
    m_video_frame_sources.ensure(id, [&] {
        return move(source);
    });
    return { id };
}

FilterResourceId DisplayListResourceStorage::add_filter(Gfx::Filter const& filter)
{
    auto id = filter.id();
    m_filters.ensure(id, [&] {
        return filter;
    });
    return { id };
}

PaintStyleResourceId DisplayListResourceStorage::add_paint_style(RefPtr<SVGPaintServerPaintStyle const> paint_style)
{
    VERIFY(paint_style);
    auto id = paint_style->id();
    m_paint_styles.ensure(id, [&] {
        return paint_style;
    });
    return { id };
}

DisplayListResourceId DisplayListResourceStorage::add_display_list(NonnullRefPtr<DisplayList const> display_list)
{
    auto id = display_list->id();
    m_display_lists.ensure(id, [&] {
        return move(display_list);
    });
    return { id };
}

void DisplayListResourceStorage::append_referenced_resources_from(
    DisplayListResourceStorage const& source,
    ReadonlySpan<DisplayListCommand> commands)
{
    for (auto const& display_list_command : commands) {
        display_list_command.visit([&](auto const& command) {
            if constexpr (requires { command.font_id; })
                add_font(source.font(command.font_id));
            if constexpr (requires { command.frame_id; })
                add_image_frame(source.image_frame(command.frame_id));
            if constexpr (requires { source.external_content_source(command.source_id); })
                add_external_content_source(source.external_content_source(command.source_id));
            if constexpr (requires { source.video_frame_source(command.source_id); })
                add_video_frame_source(source.video_frame_source(command.source_id));
            if constexpr (requires { command.paint_style_id; command.paint_kind; }) {
                if (command.paint_kind == decltype(command.paint_kind)::PaintStyle)
                    add_paint_style(source.paint_style(command.paint_style_id));
            }
            if constexpr (requires { command.backdrop_filter_id; }) {
                if (command.has_backdrop_filter)
                    add_filter(source.filter(command.backdrop_filter_id));
            }
            if constexpr (requires { command.filter_id; }) {
                if (command.has_filter)
                    add_filter(source.filter(command.filter_id));
            }
            if constexpr (requires { command.display_list_id; })
                add_display_list(source.display_list(command.display_list_id));
        });
    }
}

}
