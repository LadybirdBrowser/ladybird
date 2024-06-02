/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NumberFormat.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/MediaPaintable.h>
#include <LibWeb/UIEvents/MouseButton.h>

namespace Web::Painting {

static constexpr auto control_box_color = Gfx::Color::from_rgb(0x26'26'26);
static constexpr auto control_highlight_color = Gfx::Color::from_rgb(0x1d'99'f3);

static constexpr Gfx::Color control_button_color(bool is_hovered)
{
    if (!is_hovered)
        return Color::White;
    return control_highlight_color;
}

MediaPaintable::MediaPaintable(Layout::ReplacedBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Optional<DevicePixelPoint> MediaPaintable::mouse_position(PaintContext& context, HTML::HTMLMediaElement const& media_element)
{
    auto const& layout_mouse_position = media_element.layout_mouse_position({});

    if (layout_mouse_position.has_value() && media_element.document().hovered_node() == &media_element)
        return context.rounded_device_point(*layout_mouse_position);

    return {};
}

void MediaPaintable::fill_triangle(RecordingPainter& painter, Gfx::IntPoint location, Array<Gfx::IntPoint, 3> coordinates, Color color)
{
    Gfx::Path path;
    path.move_to((coordinates[0] + location).to_type<float>());
    path.line_to((coordinates[1] + location).to_type<float>());
    path.line_to((coordinates[2] + location).to_type<float>());
    path.close();
    painter.fill_path({
        .path = path,
        .color = color,
        .winding_rule = Gfx::Painter::WindingRule::EvenOdd,
    });
}

void MediaPaintable::paint_media_controls(PaintContext& context, HTML::HTMLMediaElement const& media_element, DevicePixelRect media_rect, Optional<DevicePixelPoint> const& mouse_position) const
{
    auto components = compute_control_bar_components(context, media_element, media_rect);
    context.recording_painter().fill_rect(components.control_box_rect.to_type<int>(), control_box_color.with_alpha(0xd0));

    paint_control_bar_playback_button(context, media_element, components, mouse_position);
    paint_control_bar_timeline(context, media_element, components);
    paint_control_bar_timestamp(context, components);
    paint_control_bar_speaker(context, media_element, components, mouse_position);
    paint_control_bar_volume(context, media_element, components, mouse_position);
}

MediaPaintable::Components MediaPaintable::compute_control_bar_components(PaintContext& context, HTML::HTMLMediaElement const& media_element, DevicePixelRect media_rect) const
{
    auto maximum_control_box_height = context.rounded_device_pixels(40);
    auto component_padding = context.rounded_device_pixels(5);

    Components components {};

    components.control_box_rect = media_rect;
    if (components.control_box_rect.height() > maximum_control_box_height)
        components.control_box_rect.take_from_top(components.control_box_rect.height() - maximum_control_box_height);

    auto remaining_rect = components.control_box_rect;
    remaining_rect.shrink(component_padding * 2, 0);

    auto timeline_rect_height = context.rounded_device_pixels(8);
    if ((timeline_rect_height * 3) <= components.control_box_rect.height()) {
        components.timeline_rect = components.control_box_rect;
        components.timeline_rect.set_height(timeline_rect_height);
        remaining_rect.take_from_top(timeline_rect_height);
    }

    auto playback_button_rect_width = min(context.rounded_device_pixels(40), remaining_rect.width());
    components.playback_button_rect = remaining_rect;
    components.playback_button_rect.set_width(playback_button_rect_width);
    remaining_rect.take_from_left(playback_button_rect_width);

    components.speaker_button_size = context.rounded_device_pixels(30);
    if (components.speaker_button_size <= remaining_rect.width()) {
        components.volume_button_size = context.rounded_device_pixels(16);

        if ((components.speaker_button_size + components.volume_button_size * 3) <= remaining_rect.width()) {
            auto volume_width = min(context.rounded_device_pixels(60), remaining_rect.width() - components.speaker_button_size);

            components.volume_rect = remaining_rect;
            components.volume_rect.take_from_left(remaining_rect.width() - volume_width);
            remaining_rect.take_from_right(volume_width);

            components.volume_scrub_rect = components.volume_rect.shrunken(components.volume_button_size, components.volume_rect.height() - components.volume_button_size / 2);
        }

        components.speaker_button_rect = remaining_rect;
        components.speaker_button_rect.take_from_left(remaining_rect.width() - components.speaker_button_size);
        remaining_rect.take_from_right(components.speaker_button_size + component_padding);
    }

    auto display_time = human_readable_digital_time(round(media_element.layout_display_time({})));
    auto duration = human_readable_digital_time(isnan(media_element.duration()) ? 0 : round(media_element.duration()));
    components.timestamp = String::formatted("{} / {}", display_time, duration).release_value_but_fixme_should_propagate_errors();
    components.timestamp_font = layout_node().scaled_font(context);

    auto timestamp_size = DevicePixels { static_cast<DevicePixels::Type>(ceilf(components.timestamp_font->width(components.timestamp))) };
    if (timestamp_size <= remaining_rect.width()) {
        components.timestamp_rect = remaining_rect;
        components.timestamp_rect.take_from_right(remaining_rect.width() - timestamp_size);
        remaining_rect.take_from_left(timestamp_size + component_padding);
    }

    media_element.cached_layout_boxes({}).control_box_rect = context.scale_to_css_rect(components.control_box_rect);
    media_element.cached_layout_boxes({}).playback_button_rect = context.scale_to_css_rect(components.playback_button_rect);
    media_element.cached_layout_boxes({}).timeline_rect = context.scale_to_css_rect(components.timeline_rect);
    media_element.cached_layout_boxes({}).speaker_button_rect = context.scale_to_css_rect(components.speaker_button_rect);
    media_element.cached_layout_boxes({}).volume_rect = context.scale_to_css_rect(components.volume_rect);
    media_element.cached_layout_boxes({}).volume_scrub_rect = context.scale_to_css_rect(components.volume_scrub_rect);

    return components;
}

void MediaPaintable::paint_control_bar_playback_button(PaintContext& context, HTML::HTMLMediaElement const& media_element, Components const& components, Optional<DevicePixelPoint> const& mouse_position)
{
    auto playback_button_size = components.playback_button_rect.width() * 4 / 10;

    auto playback_button_offset_x = (components.playback_button_rect.width() - playback_button_size) / 2;
    auto playback_button_offset_y = (components.playback_button_rect.height() - playback_button_size) / 2;
    auto playback_button_location = components.playback_button_rect.top_left().translated(playback_button_offset_x, playback_button_offset_y);

    auto playback_button_is_hovered = rect_is_hovered(media_element, components.playback_button_rect, mouse_position);
    auto playback_button_color = control_button_color(playback_button_is_hovered);

    if (media_element.paused()) {
        Array<Gfx::IntPoint, 3> play_button_coordinates { {
            { 0, 0 },
            { static_cast<int>(playback_button_size), static_cast<int>(playback_button_size) / 2 },
            { 0, static_cast<int>(playback_button_size) },
        } };

        fill_triangle(context.recording_painter(), playback_button_location.to_type<int>(), play_button_coordinates, playback_button_color);
    } else {
        DevicePixelRect pause_button_left_rect {
            playback_button_location,
            { playback_button_size / 3, playback_button_size }
        };
        DevicePixelRect pause_button_right_rect {
            playback_button_location.translated(playback_button_size * 2 / 3, 0),
            { playback_button_size / 3, playback_button_size }
        };

        context.recording_painter().fill_rect(pause_button_left_rect.to_type<int>(), playback_button_color);
        context.recording_painter().fill_rect(pause_button_right_rect.to_type<int>(), playback_button_color);
    }
}

void MediaPaintable::paint_control_bar_timeline(PaintContext& context, HTML::HTMLMediaElement const& media_element, Components const& components)
{
    if (components.timeline_rect.is_empty())
        return;

    auto playback_percentage = isnan(media_element.duration()) ? 0.0 : media_element.layout_display_time({}) / media_element.duration();
    auto playback_position = static_cast<double>(static_cast<int>(components.timeline_rect.width())) * playback_percentage;
    auto timeline_button_offset_x = static_cast<DevicePixels>(round(playback_position));

    auto timeline_past_rect = components.timeline_rect;
    timeline_past_rect.set_width(timeline_button_offset_x);
    context.recording_painter().fill_rect(timeline_past_rect.to_type<int>(), control_highlight_color.lightened());

    auto timeline_future_rect = components.timeline_rect;
    timeline_future_rect.take_from_left(timeline_button_offset_x);
    context.recording_painter().fill_rect(timeline_future_rect.to_type<int>(), Color::Black);
}

void MediaPaintable::paint_control_bar_timestamp(PaintContext& context, Components const& components)
{
    if (components.timestamp_rect.is_empty())
        return;

    context.recording_painter().draw_text(components.timestamp_rect.to_type<int>(), components.timestamp, *components.timestamp_font, Gfx::TextAlignment::CenterLeft, Color::White);
}

void MediaPaintable::paint_control_bar_speaker(PaintContext& context, HTML::HTMLMediaElement const& media_element, Components const& components, Optional<DevicePixelPoint> const& mouse_position)
{
    if (components.speaker_button_rect.is_empty())
        return;

    auto speaker_button_width = context.rounded_device_pixels(20);
    auto speaker_button_height = context.rounded_device_pixels(15);

    auto speaker_button_offset_x = (components.speaker_button_rect.width() - speaker_button_width) / 2;
    auto speaker_button_offset_y = (components.speaker_button_rect.height() - speaker_button_height) / 2;
    auto speaker_button_location = components.speaker_button_rect.top_left().translated(speaker_button_offset_x, speaker_button_offset_y);

    auto device_point = [&](double x, double y) {
        auto position = context.rounded_device_point({ x, y }) + speaker_button_location;
        return position.to_type<DevicePixels::Type>().to_type<float>();
    };

    auto speaker_button_is_hovered = rect_is_hovered(media_element, components.speaker_button_rect, mouse_position);
    auto speaker_button_color = control_button_color(speaker_button_is_hovered);

    Gfx::Path path;

    path.move_to(device_point(0, 4));
    path.line_to(device_point(5, 4));
    path.line_to(device_point(11, 0));
    path.line_to(device_point(11, 15));
    path.line_to(device_point(5, 11));
    path.line_to(device_point(0, 11));
    path.line_to(device_point(0, 4));
    path.close();
    context.recording_painter().fill_path({ .path = path, .color = speaker_button_color, .winding_rule = Gfx::Painter::WindingRule::EvenOdd });

    path.clear();
    path.move_to(device_point(13, 3));
    path.quadratic_bezier_curve_to(device_point(16, 7.5), device_point(13, 12));
    path.move_to(device_point(14, 0));
    path.quadratic_bezier_curve_to(device_point(20, 7.5), device_point(14, 15));
    context.recording_painter().stroke_path({ .path = path, .color = speaker_button_color, .thickness = 1 });

    if (media_element.muted()) {
        context.recording_painter().draw_line(device_point(0, 0).to_type<int>(), device_point(20, 15).to_type<int>(), Color::Red, 2);
        context.recording_painter().draw_line(device_point(0, 15).to_type<int>(), device_point(20, 0).to_type<int>(), Color::Red, 2);
    }
}

void MediaPaintable::paint_control_bar_volume(PaintContext& context, HTML::HTMLMediaElement const& media_element, Components const& components, Optional<DevicePixelPoint> const& mouse_position)
{
    if (components.volume_rect.is_empty())
        return;

    auto volume_position = static_cast<double>(static_cast<int>(components.volume_scrub_rect.width())) * media_element.volume();
    auto volume_button_offset_x = static_cast<DevicePixels>(round(volume_position));

    auto volume_lower_rect = components.volume_scrub_rect;
    volume_lower_rect.set_width(volume_button_offset_x);
    context.recording_painter().fill_rect_with_rounded_corners(volume_lower_rect.to_type<int>(), control_highlight_color.lightened(), 4);

    auto volume_higher_rect = components.volume_scrub_rect;
    volume_higher_rect.take_from_left(volume_button_offset_x);
    context.recording_painter().fill_rect_with_rounded_corners(volume_higher_rect.to_type<int>(), Color::Black, 4);

    auto volume_button_rect = components.volume_scrub_rect;
    volume_button_rect.shrink(components.volume_scrub_rect.width() - components.volume_button_size, components.volume_scrub_rect.height() - components.volume_button_size);
    volume_button_rect.set_x(components.volume_scrub_rect.x() + volume_button_offset_x - components.volume_button_size / 2);

    auto volume_is_hovered = rect_is_hovered(media_element, components.volume_rect, mouse_position, HTML::HTMLMediaElement::MouseTrackingComponent::Volume);
    auto volume_color = control_button_color(volume_is_hovered);
    context.recording_painter().fill_ellipse(volume_button_rect.to_type<int>(), volume_color);
}

MediaPaintable::DispatchEventOfSameName MediaPaintable::handle_mousedown(Badge<EventHandler>, CSSPixelPoint position, unsigned button, unsigned)
{
    if (button != UIEvents::MouseButton::Primary)
        return DispatchEventOfSameName::Yes;

    auto& media_element = *verify_cast<HTML::HTMLMediaElement>(layout_box().dom_node());
    auto const& cached_layout_boxes = media_element.cached_layout_boxes({});

    if (cached_layout_boxes.timeline_rect.has_value() && cached_layout_boxes.timeline_rect->contains(position)) {
        media_element.set_layout_mouse_tracking_component({}, HTML::HTMLMediaElement::MouseTrackingComponent::Timeline);
        set_current_time(media_element, *cached_layout_boxes.timeline_rect, position, Temporary::Yes);
    } else if (cached_layout_boxes.volume_rect.has_value() && cached_layout_boxes.volume_rect->contains(position)) {
        media_element.set_layout_mouse_tracking_component({}, HTML::HTMLMediaElement::MouseTrackingComponent::Volume);
        set_volume(media_element, *cached_layout_boxes.volume_scrub_rect, position);
    }

    if (media_element.layout_mouse_tracking_component({}).has_value())
        const_cast<HTML::Navigable&>(*navigable()).event_handler().set_mouse_event_tracking_paintable(this);

    return DispatchEventOfSameName::Yes;
}

MediaPaintable::DispatchEventOfSameName MediaPaintable::handle_mouseup(Badge<EventHandler>, CSSPixelPoint position, unsigned button, unsigned)
{
    auto& media_element = *verify_cast<HTML::HTMLMediaElement>(layout_box().dom_node());
    auto const& cached_layout_boxes = media_element.cached_layout_boxes({});

    if (auto const& mouse_tracking_component = media_element.layout_mouse_tracking_component({}); mouse_tracking_component.has_value()) {
        switch (*mouse_tracking_component) {
        case HTML::HTMLMediaElement::MouseTrackingComponent::Timeline:
            set_current_time(media_element, *cached_layout_boxes.timeline_rect, position, Temporary::No);
            media_element.set_layout_display_time({}, {});
            break;

        case HTML::HTMLMediaElement::MouseTrackingComponent::Volume:
            browsing_context().page().client().page_did_leave_tooltip_area();
            break;
        }

        const_cast<HTML::Navigable&>(*navigable()).event_handler().set_mouse_event_tracking_paintable(nullptr);
        media_element.set_layout_mouse_tracking_component({}, {});

        return DispatchEventOfSameName::Yes;
    }

    if (button != UIEvents::MouseButton::Primary)
        return DispatchEventOfSameName::Yes;

    if (cached_layout_boxes.control_box_rect.has_value() && cached_layout_boxes.control_box_rect->contains(position)) {
        if (cached_layout_boxes.playback_button_rect.has_value() && cached_layout_boxes.playback_button_rect->contains(position)) {
            media_element.toggle_playback().release_value_but_fixme_should_propagate_errors();
            return DispatchEventOfSameName::Yes;
        }

        if (cached_layout_boxes.speaker_button_rect.has_value() && cached_layout_boxes.speaker_button_rect->contains(position)) {
            media_element.set_muted(!media_element.muted());
            return DispatchEventOfSameName::Yes;
        }

        if (cached_layout_boxes.timeline_rect.has_value() && cached_layout_boxes.timeline_rect->contains(position))
            return DispatchEventOfSameName::No;
    }

    media_element.toggle_playback().release_value_but_fixme_should_propagate_errors();
    return DispatchEventOfSameName::Yes;
}

MediaPaintable::DispatchEventOfSameName MediaPaintable::handle_mousemove(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    auto& media_element = *verify_cast<HTML::HTMLMediaElement>(layout_box().dom_node());
    auto const& cached_layout_boxes = media_element.cached_layout_boxes({});

    if (auto const& mouse_tracking_component = media_element.layout_mouse_tracking_component({}); mouse_tracking_component.has_value()) {
        switch (*mouse_tracking_component) {
        case HTML::HTMLMediaElement::MouseTrackingComponent::Timeline:
            if (cached_layout_boxes.timeline_rect.has_value())
                set_current_time(media_element, *cached_layout_boxes.timeline_rect, position, Temporary::Yes);
            break;

        case HTML::HTMLMediaElement::MouseTrackingComponent::Volume:
            if (cached_layout_boxes.volume_rect.has_value()) {
                set_volume(media_element, *cached_layout_boxes.volume_scrub_rect, position);

                auto volume = static_cast<u8>(media_element.volume() * 100.0);
                browsing_context().page().client().page_did_enter_tooltip_area(position, ByteString::formatted("{}%", volume));
            }

            break;
        }
    }

    if (absolute_rect().contains(position)) {
        media_element.set_layout_mouse_position({}, position);
        return DispatchEventOfSameName::Yes;
    }

    media_element.set_layout_mouse_position({}, {});
    return DispatchEventOfSameName::No;
}

void MediaPaintable::set_current_time(HTML::HTMLMediaElement& media_element, CSSPixelRect timeline_rect, CSSPixelPoint mouse_position, Temporary temporarily)
{
    auto x_offset = mouse_position.x() - timeline_rect.x();
    x_offset = max(x_offset, 0);
    x_offset = min(x_offset, timeline_rect.width());

    auto x_percentage = static_cast<double>(x_offset) / static_cast<double>(timeline_rect.width());
    auto position = static_cast<double>(x_percentage) * media_element.duration();

    switch (temporarily) {
    case Temporary::Yes:
        media_element.set_layout_display_time({}, position);
        break;
    case Temporary::No:
        media_element.set_current_time(position);
        break;
    }
}

void MediaPaintable::set_volume(HTML::HTMLMediaElement& media_element, CSSPixelRect volume_rect, CSSPixelPoint mouse_position)
{
    auto x_offset = mouse_position.x() - volume_rect.x();
    x_offset = max(x_offset, 0);
    x_offset = min(x_offset, volume_rect.width());

    auto volume = static_cast<double>(x_offset) / static_cast<double>(volume_rect.width());
    media_element.set_volume(volume).release_value_but_fixme_should_propagate_errors();
}

bool MediaPaintable::rect_is_hovered(HTML::HTMLMediaElement const& media_element, Optional<DevicePixelRect> const& rect, Optional<DevicePixelPoint> const& mouse_position, Optional<HTML::HTMLMediaElement::MouseTrackingComponent> const& allowed_mouse_tracking_component)
{
    if (auto const& mouse_tracking_component = media_element.layout_mouse_tracking_component({}); mouse_tracking_component.has_value())
        return mouse_tracking_component == allowed_mouse_tracking_component;

    if (!rect.has_value() || !mouse_position.has_value())
        return false;

    return rect->contains(*mouse_position);
}

}
