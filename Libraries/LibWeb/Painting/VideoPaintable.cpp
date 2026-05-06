/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Gregory Bertilso <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/Layout/VideoBox.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/Painting/VideoPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(VideoPaintable);

GC::Ref<VideoPaintable> VideoPaintable::create(Layout::VideoBox const& layout_box)
{
    return layout_box.heap().allocate<VideoPaintable>(layout_box);
}

VideoPaintable::VideoPaintable(Layout::VideoBox const& layout_box)
    : PaintableBox(layout_box)
{
}

void VideoPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    Base::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    DisplayListRecorderStateSaver saver { context.display_list_recorder() };

    auto video_rect = context.rounded_device_rect(absolute_rect());
    context.display_list_recorder().add_clip_rect(video_rect.to_type<int>());

    ScopedCornerRadiusClip corner_clip { context, video_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

    auto const& video_element = as<HTML::HTMLVideoElement>(*dom_node());

    auto const& poster_frame = video_element.poster_frame();

    auto paint_frame = [&](Gfx::DecodedImageFrame const& frame) {
        auto dst_rect = get_replaced_box_painting_area(*this, context, computed_values().object_fit(), frame.size());
        if (dst_rect.is_empty())
            return;
        auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), frame.size(), dst_rect.size());
        context.display_list_recorder().draw_scaled_decoded_image_frame(dst_rect, dst_rect, frame, scaling_mode);
    };

    auto paint_video_frame = [&]() {
        auto current_frame = video_element.current_decoded_image_frame();
        if (!current_frame)
            return;
        paint_frame(*current_frame);
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
        paint_frame(*Gfx::DecodedImageFrame::create(*poster_frame));
        break;

    case HTML::HTMLVideoElement::Representation::TransparentBlack:
        paint_transparent_black();
        break;
    }
}

}
