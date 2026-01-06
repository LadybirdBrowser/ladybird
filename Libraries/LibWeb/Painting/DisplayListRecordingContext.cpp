/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web {

static u64 s_next_paint_generation_id = 0;

DisplayListRecordingContext::DisplayListRecordingContext(Painting::DisplayListRecorder& display_list_recorder, Palette const& palette, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics)
    : m_display_list_recorder(display_list_recorder)
    , m_palette(palette)
    , m_device_pixel_converter(device_pixels_per_css_pixel)
    , m_chrome_metrics(chrome_metrics)
    , m_paint_generation_id(s_next_paint_generation_id++)
{
}

CSSPixelRect DisplayListRecordingContext::css_viewport_rect() const
{
    return {
        m_device_viewport_rect.x().value() / m_device_pixel_converter.device_pixels_per_css_pixel(),
        m_device_viewport_rect.y().value() / m_device_pixel_converter.device_pixels_per_css_pixel(),
        m_device_viewport_rect.width().value() / m_device_pixel_converter.device_pixels_per_css_pixel(),
        m_device_viewport_rect.height().value() / m_device_pixel_converter.device_pixels_per_css_pixel()
    };
}

DevicePixels DisplayListRecordingContext::rounded_device_pixels(CSSPixels css_pixels) const
{
    return m_device_pixel_converter.rounded_device_pixels(css_pixels);
}

DevicePixels DisplayListRecordingContext::enclosing_device_pixels(CSSPixels css_pixels) const
{
    return m_device_pixel_converter.enclosing_device_pixels(css_pixels);
}

DevicePixels DisplayListRecordingContext::floored_device_pixels(CSSPixels css_pixels) const
{
    return m_device_pixel_converter.floored_device_pixels(css_pixels);
}

DevicePixelPoint DisplayListRecordingContext::rounded_device_point(CSSPixelPoint point) const
{
    return m_device_pixel_converter.rounded_device_point(point);
}

DevicePixelPoint DisplayListRecordingContext::floored_device_point(CSSPixelPoint point) const
{
    return m_device_pixel_converter.floored_device_point(point);
}

DevicePixelRect DisplayListRecordingContext::enclosing_device_rect(CSSPixelRect rect) const
{
    return m_device_pixel_converter.enclosing_device_rect(rect);
}

DevicePixelRect DisplayListRecordingContext::rounded_device_rect(CSSPixelRect rect) const
{
    return m_device_pixel_converter.rounded_device_rect(rect);
}

DevicePixelSize DisplayListRecordingContext::enclosing_device_size(CSSPixelSize size) const
{
    return m_device_pixel_converter.enclosing_device_size(size);
}

DevicePixelSize DisplayListRecordingContext::rounded_device_size(CSSPixelSize size) const
{
    return m_device_pixel_converter.rounded_device_size(size);
}

CSSPixels DisplayListRecordingContext::scale_to_css_pixels(DevicePixels device_pixels) const
{
    return CSSPixels::nearest_value_for(device_pixels.value() / m_device_pixel_converter.device_pixels_per_css_pixel());
}

CSSPixelPoint DisplayListRecordingContext::scale_to_css_point(DevicePixelPoint point) const
{
    return {
        scale_to_css_pixels(point.x()),
        scale_to_css_pixels(point.y())
    };
}

CSSPixelSize DisplayListRecordingContext::scale_to_css_size(DevicePixelSize size) const
{
    return {
        scale_to_css_pixels(size.width()),
        scale_to_css_pixels(size.height())
    };
}

CSSPixelRect DisplayListRecordingContext::scale_to_css_rect(DevicePixelRect rect) const
{
    return {
        scale_to_css_point(rect.location()),
        scale_to_css_size(rect.size())
    };
}

}
