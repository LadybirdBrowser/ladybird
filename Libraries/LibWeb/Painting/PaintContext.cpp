/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/PaintContext.h>

namespace Web {

static u64 s_next_paint_generation_id = 0;

PaintContext::PaintContext(Painting::DisplayListRecorder& display_list_recorder, Palette const& palette, double device_pixels_per_css_pixel)
    : m_display_list_recorder(display_list_recorder)
    , m_palette(palette)
    , m_device_pixel_converter(device_pixels_per_css_pixel)
    , m_paint_generation_id(s_next_paint_generation_id++)
{
}

CSSPixelRect PaintContext::css_viewport_rect() const
{
    return {
        m_device_viewport_rect.x().value() / m_device_pixel_converter.device_pixels_per_css_pixel(),
        m_device_viewport_rect.y().value() / m_device_pixel_converter.device_pixels_per_css_pixel(),
        m_device_viewport_rect.width().value() / m_device_pixel_converter.device_pixels_per_css_pixel(),
        m_device_viewport_rect.height().value() / m_device_pixel_converter.device_pixels_per_css_pixel()
    };
}

DevicePixels PaintContext::rounded_device_pixels(CSSPixels css_pixels) const
{
    return m_device_pixel_converter.rounded_device_pixels(css_pixels);
}

DevicePixels PaintContext::enclosing_device_pixels(CSSPixels css_pixels) const
{
    return m_device_pixel_converter.enclosing_device_pixels(css_pixels);
}

DevicePixels PaintContext::floored_device_pixels(CSSPixels css_pixels) const
{
    return m_device_pixel_converter.floored_device_pixels(css_pixels);
}

DevicePixelPoint PaintContext::rounded_device_point(CSSPixelPoint point) const
{
    return m_device_pixel_converter.rounded_device_point(point);
}

DevicePixelPoint PaintContext::floored_device_point(CSSPixelPoint point) const
{
    return m_device_pixel_converter.floored_device_point(point);
}

DevicePixelRect PaintContext::enclosing_device_rect(CSSPixelRect rect) const
{
    return m_device_pixel_converter.enclosing_device_rect(rect);
}

DevicePixelRect PaintContext::rounded_device_rect(CSSPixelRect rect) const
{
    return m_device_pixel_converter.rounded_device_rect(rect);
}

DevicePixelSize PaintContext::enclosing_device_size(CSSPixelSize size) const
{
    return m_device_pixel_converter.enclosing_device_size(size);
}

DevicePixelSize PaintContext::rounded_device_size(CSSPixelSize size) const
{
    return m_device_pixel_converter.rounded_device_size(size);
}

CSSPixels PaintContext::scale_to_css_pixels(DevicePixels device_pixels) const
{
    return CSSPixels::nearest_value_for(device_pixels.value() / m_device_pixel_converter.device_pixels_per_css_pixel());
}

CSSPixelPoint PaintContext::scale_to_css_point(DevicePixelPoint point) const
{
    return {
        scale_to_css_pixels(point.x()),
        scale_to_css_pixels(point.y())
    };
}

CSSPixelSize PaintContext::scale_to_css_size(DevicePixelSize size) const
{
    return {
        scale_to_css_pixels(size.width()),
        scale_to_css_pixels(size.height())
    };
}

CSSPixelRect PaintContext::scale_to_css_rect(DevicePixelRect rect) const
{
    return {
        scale_to_css_point(rect.location()),
        scale_to_css_size(rect.size())
    };
}

}
