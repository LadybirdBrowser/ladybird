/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class DevicePixelConverter {
public:
    DevicePixels rounded_device_pixels(CSSPixels css_pixels) const
    {
        return round(css_pixels.to_double() * m_device_pixels_per_css_pixel);
    }

    DevicePixels enclosing_device_pixels(CSSPixels css_pixels) const
    {
        return ceil(css_pixels.to_double() * m_device_pixels_per_css_pixel);
    }

    DevicePixels floored_device_pixels(CSSPixels css_pixels) const
    {
        return floor(css_pixels.to_double() * m_device_pixels_per_css_pixel);
    }

    DevicePixelPoint rounded_device_point(CSSPixelPoint point) const
    {
        return {
            round(point.x().to_double() * m_device_pixels_per_css_pixel),
            round(point.y().to_double() * m_device_pixels_per_css_pixel)
        };
    }

    DevicePixelPoint floored_device_point(CSSPixelPoint point) const
    {
        return {
            floor(point.x().to_double() * m_device_pixels_per_css_pixel),
            floor(point.y().to_double() * m_device_pixels_per_css_pixel)
        };
    }

    DevicePixelRect enclosing_device_rect(CSSPixelRect rect) const
    {
        return {
            floor(rect.x().to_double() * m_device_pixels_per_css_pixel),
            floor(rect.y().to_double() * m_device_pixels_per_css_pixel),
            ceil(rect.width().to_double() * m_device_pixels_per_css_pixel),
            ceil(rect.height().to_double() * m_device_pixels_per_css_pixel)
        };
    }

    DevicePixelRect rounded_device_rect(CSSPixelRect rect) const
    {
        auto scaled_rect = rect.to_type<double>().scaled(m_device_pixels_per_css_pixel);
        auto x = round(scaled_rect.x());
        auto y = round(scaled_rect.y());
        return { x, y, round(scaled_rect.right()) - x, round(scaled_rect.bottom()) - y };
    }

    DevicePixelSize enclosing_device_size(CSSPixelSize size) const
    {
        return {
            ceil(size.width().to_double() * m_device_pixels_per_css_pixel),
            ceil(size.height().to_double() * m_device_pixels_per_css_pixel)
        };
    }

    DevicePixelSize rounded_device_size(CSSPixelSize size) const
    {
        return {
            round(size.width().to_double() * m_device_pixels_per_css_pixel),
            round(size.height().to_double() * m_device_pixels_per_css_pixel)
        };
    }

    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }

    DevicePixelConverter(double device_pixels_per_css_pixel)
        : m_device_pixels_per_css_pixel(device_pixels_per_css_pixel)
    {
    }

private:
    double m_device_pixels_per_css_pixel { 0 };
};

}
