/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

class PaintContext {
public:
    PaintContext(Painting::DisplayListRecorder& painter, Palette const& palette, double device_pixels_per_css_pixel);

    Painting::DisplayListRecorder& display_list_recorder() const { return m_display_list_recorder; }
    Palette const& palette() const { return m_palette; }

    bool should_show_line_box_borders() const { return m_should_show_line_box_borders; }
    void set_should_show_line_box_borders(bool value) { m_should_show_line_box_borders = value; }

    bool should_paint_overlay() const { return m_should_paint_overlay; }
    void set_should_paint_overlay(bool should_paint_overlay) { m_should_paint_overlay = should_paint_overlay; }

    DevicePixelRect device_viewport_rect() const { return m_device_viewport_rect; }
    void set_device_viewport_rect(DevicePixelRect const& rect) { m_device_viewport_rect = rect; }
    CSSPixelRect css_viewport_rect() const;

    bool has_focus() const { return m_focus; }
    void set_has_focus(bool focus) { m_focus = focus; }

    void set_svg_transform(Gfx::AffineTransform transform)
    {
        m_svg_transform = transform;
    }

    Gfx::AffineTransform const& svg_transform() const
    {
        return m_svg_transform;
    }

    bool draw_svg_geometry_for_clip_path() const
    {
        return m_draw_svg_geometry_for_clip_path;
    }

    void set_draw_svg_geometry_for_clip_path(bool draw_svg_geometry_for_clip_path)
    {
        m_draw_svg_geometry_for_clip_path = draw_svg_geometry_for_clip_path;
    }

    DevicePixels enclosing_device_pixels(CSSPixels css_pixels) const;
    DevicePixels floored_device_pixels(CSSPixels css_pixels) const;
    DevicePixels rounded_device_pixels(CSSPixels css_pixels) const;
    DevicePixelPoint rounded_device_point(CSSPixelPoint) const;
    DevicePixelPoint floored_device_point(CSSPixelPoint) const;
    DevicePixelRect enclosing_device_rect(CSSPixelRect) const;
    DevicePixelRect rounded_device_rect(CSSPixelRect) const;
    DevicePixelSize enclosing_device_size(CSSPixelSize) const;
    DevicePixelSize rounded_device_size(CSSPixelSize) const;
    CSSPixels scale_to_css_pixels(DevicePixels) const;
    CSSPixelPoint scale_to_css_point(DevicePixelPoint) const;
    CSSPixelSize scale_to_css_size(DevicePixelSize) const;
    CSSPixelRect scale_to_css_rect(DevicePixelRect) const;

    PaintContext clone(Painting::DisplayListRecorder& painter) const
    {
        auto clone = PaintContext(painter, m_palette, m_device_pixels_per_css_pixel);
        clone.m_device_viewport_rect = m_device_viewport_rect;
        clone.m_should_show_line_box_borders = m_should_show_line_box_borders;
        clone.m_should_paint_overlay = m_should_paint_overlay;
        clone.m_focus = m_focus;
        return clone;
    }

    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }

    u32 allocate_corner_clipper_id() { return m_next_corner_clipper_id++; }

    u64 paint_generation_id() const { return m_paint_generation_id; }

private:
    Painting::DisplayListRecorder& m_display_list_recorder;
    Palette m_palette;
    double m_device_pixels_per_css_pixel { 0 };
    DevicePixelRect m_device_viewport_rect;
    bool m_should_show_line_box_borders { false };
    bool m_should_paint_overlay { true };
    bool m_focus { false };
    bool m_draw_svg_geometry_for_clip_path { false };
    Gfx::AffineTransform m_svg_transform;
    u32 m_next_corner_clipper_id { 0 };
    u64 m_paint_generation_id { 0 };
};

}
