/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class MediaPaintable : public PaintableBox {
    GC_CELL(MediaPaintable, PaintableBox);

protected:
    explicit MediaPaintable(Layout::ReplacedBox const&);

    static Optional<DevicePixelPoint> mouse_position(DisplayListRecordingContext&, HTML::HTMLMediaElement const&);
    static void fill_triangle(DisplayListRecorder& painter, Gfx::IntPoint location, Array<Gfx::IntPoint, 3> coordinates, Color color);

    void paint_media_controls(DisplayListRecordingContext&, HTML::HTMLMediaElement const&, DevicePixelRect media_rect, Optional<DevicePixelPoint> const& mouse_position) const;

private:
    struct Components {
        DevicePixelRect control_box_rect;
        DevicePixelRect playback_button_rect;
        DevicePixelRect timeline_rect;

        Utf16String timestamp;
        RefPtr<Gfx::Font const> timestamp_font;
        DevicePixelRect timestamp_rect;

        DevicePixelRect speaker_button_rect;
        DevicePixels speaker_button_size;

        DevicePixelRect volume_rect;
        DevicePixelRect volume_scrub_rect;
        DevicePixels volume_button_size;
    };

    virtual bool wants_mouse_events() const override { return true; }
    virtual DispatchEventOfSameName handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers) override;

    Components compute_control_bar_components(DisplayListRecordingContext&, HTML::HTMLMediaElement const&, DevicePixelRect media_rect) const;
    static void paint_control_bar_playback_button(DisplayListRecordingContext&, HTML::HTMLMediaElement const&, Components const&, Optional<DevicePixelPoint> const& mouse_position);
    static void paint_control_bar_timeline(DisplayListRecordingContext&, HTML::HTMLMediaElement const&, Components const&);
    static void paint_control_bar_timestamp(DisplayListRecordingContext&, Components const&);
    static void paint_control_bar_speaker(DisplayListRecordingContext&, HTML::HTMLMediaElement const&, Components const& components, Optional<DevicePixelPoint> const& mouse_position);
    static void paint_control_bar_volume(DisplayListRecordingContext&, HTML::HTMLMediaElement const&, Components const&, Optional<DevicePixelPoint> const& mouse_position);

    enum class Temporary {
        Yes,
        No,
    };
    static void set_current_time(HTML::HTMLMediaElement& media_element, CSSPixelRect timeline_rect, CSSPixelPoint mouse_position, Temporary);
    static void set_volume(HTML::HTMLMediaElement& media_element, CSSPixelRect volume_rect, CSSPixelPoint mouse_position);

    static bool rect_is_hovered(HTML::HTMLMediaElement const& media_element, Optional<DevicePixelRect> const& rect, Optional<DevicePixelPoint> const& mouse_position, Optional<HTML::HTMLMediaElement::MediaComponent> const& allowed_mouse_tracking_component = {});
};

}
