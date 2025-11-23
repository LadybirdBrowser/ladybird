/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/Layout/VideoBox.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/VideoPaintable.h>

namespace Web::Painting {

static constexpr auto control_box_color = Gfx::Color::from_bgrx(0x26'26'26);
static constexpr auto control_highlight_color = Gfx::Color::from_bgrx(0x1d'99'f3);

GC_DEFINE_ALLOCATOR(VideoPaintable);

static constexpr Gfx::Color control_button_color(bool is_hovered)
{
    if (!is_hovered)
        return Color::White;
    return control_highlight_color;
}

GC::Ref<VideoPaintable> VideoPaintable::create(Layout::VideoBox const& layout_box)
{
    return layout_box.heap().allocate<VideoPaintable>(layout_box);
}

VideoPaintable::VideoPaintable(Layout::VideoBox const& layout_box)
    : MediaPaintable(layout_box)
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
    auto mouse_position = MediaPaintable::mouse_position(context, video_element);

    auto const& current_frame = video_element.selected_video_track_sink() != nullptr ? video_element.selected_video_track_sink()->current_frame() : nullptr;
    auto const& poster_frame = video_element.poster_frame();

    auto current_playback_position = video_element.current_playback_position();
    auto ready_state = video_element.ready_state();

    // NOTE: We combine the values of...
    //       - The first frame of the video
    //       - The last frame of the video to have been rendered
    //       - The frame of video corresponding to the current playback position
    //       ...into the value of VideoFrame below, as the playback system itself implements
    //       the details of the selection of a video frame to match the specification in this
    //       respect.
    enum class Representation : u8 {
        VideoFrame,
        PosterFrame,
        TransparentBlack,
    };

    auto representation = [&]() {
        // https://html.spec.whatwg.org/multipage/media.html#the-video-element:the-video-element-7
        // A video element represents what is given for the first matching condition in the list below:

        // -> When no video data is available (the element's readyState attribute is either HAVE_NOTHING, or HAVE_METADATA
        //    but no video data has yet been obtained at all, or the element's readyState attribute is any subsequent value
        //    but the media resource does not have a video channel)
        if (ready_state == HTML::HTMLMediaElement::ReadyState::HaveNothing
            || (ready_state >= HTML::HTMLMediaElement::ReadyState::HaveMetadata && video_element.video_tracks()->length() == 0)) {
            // The video element represents its poster frame, if any, or else transparent black with no intrinsic dimensions.
            return poster_frame ? Representation::PosterFrame : Representation::TransparentBlack;
        }

        // -> When the video element is paused, the current playback position is the first frame of video, and the element's
        //    show poster flag is set
        if (video_element.paused() && current_playback_position == 0 && video_element.show_poster()) {
            // The video element represents its poster frame, if any, or else the first frame of the video.
            return poster_frame ? Representation::PosterFrame : Representation::VideoFrame;
        }

        // -> When the video element is paused, and the frame of video corresponding to the current playback position
        //    is not available (e.g. because the video is seeking or buffering)
        //
        //     The video element represents the last frame of the video to have been rendered.
        //
        // NOTE: We don't need to check this condition, as seeking is asynchronous, and the last available frame
        //       will be kept until the seek completes.

        // -> When the video element is neither potentially playing nor paused (e.g. when seeking or stalled)
        if (!video_element.potentially_playing() && !video_element.paused()) {
            // The video element represents the last frame of the video to have been rendered.
            return Representation::VideoFrame;
        }

        // -> When the video element is paused
        if (video_element.paused()) {
            // The video element represents the frame of video corresponding to the current playback position.
            return Representation::VideoFrame;
        }

        // -> Otherwise (the video element has a video channel and is potentially playing)
        //
        //     The video element represents the frame of video at the continuously increasing "current" position. When the
        //     current playback position changes such that the last frame rendered is no longer the frame corresponding to
        //     the current playback position in the video, the new frame must be rendered.
        return Representation::VideoFrame;
    }();

    auto paint_frame = [&](auto const& frame) {
        auto dst_rect = video_rect.to_type<int>();
        auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), frame->rect().size(), dst_rect.size());
        context.display_list_recorder().draw_scaled_immutable_bitmap(dst_rect, dst_rect, Gfx::ImmutableBitmap::create(*frame), scaling_mode);
    };

    auto paint_transparent_black = [&]() {
        static constexpr auto transparent_black = Gfx::Color::from_bgra(0x00'00'00'00);
        context.display_list_recorder().fill_rect(video_rect.to_type<int>(), transparent_black);
    };

    auto paint_loaded_video_controls = [&]() {
        auto is_hovered = document().hovered_node() == &video_element;
        auto is_paused = video_element.paused();

        if (is_hovered || is_paused)
            paint_media_controls(context, video_element, video_rect, mouse_position);
    };

    auto paint_user_agent_controls = video_element.has_attribute(HTML::AttributeNames::controls) || video_element.is_scripting_disabled();

    switch (representation) {
    case Representation::VideoFrame:
        if (current_frame)
            paint_frame(current_frame);
        if (paint_user_agent_controls)
            paint_loaded_video_controls();
        break;

    case Representation::PosterFrame:
        VERIFY(poster_frame);
        paint_frame(poster_frame);
        if (paint_user_agent_controls)
            paint_placeholder_video_controls(context, video_rect, mouse_position);
        break;

    case Representation::TransparentBlack:
        paint_transparent_black();
        if (paint_user_agent_controls)
            paint_placeholder_video_controls(context, video_rect, mouse_position);
        break;
    }
}

void VideoPaintable::paint_placeholder_video_controls(DisplayListRecordingContext& context, DevicePixelRect video_rect, Optional<DevicePixelPoint> const& mouse_position) const
{
    auto maximum_control_box_size = context.rounded_device_pixels(100);
    auto maximum_playback_button_size = context.rounded_device_pixels(40);

    auto center = video_rect.center();

    auto control_box_size = min(maximum_control_box_size, min(video_rect.width(), video_rect.height()) * 4 / 5);
    auto control_box_offset_x = control_box_size / 2;
    auto control_box_offset_y = control_box_size / 2;

    auto control_box_location = center.translated(-control_box_offset_x, -control_box_offset_y);
    DevicePixelRect control_box_rect { control_box_location, { control_box_size, control_box_size } };

    auto playback_button_size = min(maximum_playback_button_size, min(video_rect.width(), video_rect.height()) * 2 / 5);
    auto playback_button_offset_x = playback_button_size / 2;
    auto playback_button_offset_y = playback_button_size / 2;

    // We want to center the play button on its center of mass, which is not the midpoint of its vertices.
    // To do so, reduce its desired x offset by a factor of tan(30 degrees) / 2 (about 0.288685).
    playback_button_offset_x -= 0.288685f * static_cast<float>(static_cast<DevicePixels::Type>(playback_button_offset_x));

    auto playback_button_location = center.translated(-playback_button_offset_x, -playback_button_offset_y);

    Array<Gfx::IntPoint, 3> play_button_coordinates { {
        { 0, 0 },
        { static_cast<int>(playback_button_size), static_cast<int>(playback_button_size) / 2 },
        { 0, static_cast<int>(playback_button_size) },
    } };

    auto playback_button_is_hovered = mouse_position.has_value() && control_box_rect.contains(*mouse_position);
    auto playback_button_color = control_button_color(playback_button_is_hovered);

    context.display_list_recorder().fill_ellipse(control_box_rect.to_type<int>(), control_box_color);
    fill_triangle(context.display_list_recorder(), playback_button_location.to_type<int>(), play_button_coordinates, playback_button_color);
}

}
