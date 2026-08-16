/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Gregory Bertilso <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/Painting/VideoPaintable.h>

namespace Web::Painting {

static CSSPixelSize to_css_pixel_size(Gfx::IntSize size)
{
    return CSSPixelSize { CSSPixels(size.width()), CSSPixels(size.height()) };
}

NonnullRefPtr<VideoPaintable> VideoPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new VideoPaintable(layout_box));
}

VideoPaintable::VideoPaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

void VideoPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    Paintable::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    DisplayListRecorderStateSaver saver { context.display_list_recorder() };

    auto video_rect = context.rounded_device_rect(absolute_rect());
    context.display_list_recorder().add_clip_rect(video_rect.to_type<int>());

    ScopedCornerRadiusClip corner_clip { context, video_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

    auto const& video_element = as<HTML::HTMLVideoElement>(*dom_node());

    auto const& poster_frame = video_element.poster_frame();

    auto paint_bitmap = [&](auto const& bitmap) {
        auto frame = Gfx::DecodedImageFrame { bitmap };
        auto dst_rect = get_replaced_box_painting_area(*this, context, layout_node().object_fit(), to_css_pixel_size(bitmap.size()));
        if (dst_rect.is_empty())
            return;
        auto scaling_mode = to_gfx_scaling_mode(layout_node().image_rendering(), frame.size(), dst_rect.size());
        context.display_list_recorder().draw_scaled_decoded_image_frame(dst_rect, move(frame), scaling_mode);
    };

    auto paint_video_frame = [&]() {
        auto sink_handle = video_element.video_sink_handle();
        if (!sink_handle.has_value() || !video_element.natural_media_size().has_value())
            return;
        auto src_size = video_element.natural_media_size()->to_type<int>();

        auto dst_rect = get_replaced_box_painting_area(*this, context, layout_node().object_fit(), to_css_pixel_size(src_size));
        if (dst_rect.is_empty())
            return;
        auto scaling_mode = to_gfx_scaling_mode(layout_node().image_rendering(), src_size, dst_rect.size());
        auto video_sink_id = video_element.video_sink_resource_id().value();
        context.display_list_recorder().draw_video_frame(dst_rect, video_sink_id, *sink_handle, scaling_mode);
    };

    auto paint_transparent_black = [&]() {
        static constexpr auto transparent_black = Gfx::Color::from_bgra(0x00'00'00'00);
        context.display_list_recorder().fill_rect(video_rect.to_type<int>(), transparent_black);
    };

    auto representation = video_element.current_representation();

    switch (representation) {
    case HTML::HTMLVideoElement::Representation::FirstVideoFrame:
    case HTML::HTMLVideoElement::Representation::VideoFrame:
        paint_video_frame();
        break;

    case HTML::HTMLVideoElement::Representation::PosterFrame:
        VERIFY(poster_frame);
        paint_bitmap(*poster_frame);
        break;

    case HTML::HTMLVideoElement::Representation::TransparentBlack:
        paint_transparent_black();
        break;
    }
}

}
