/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/DevicePixelConverter.h>
#include <LibWeb/Painting/DisplayListCommandRange.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/VisualContextIndex.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class AccumulatedVisualContextTree;
class DisplayList;
class ScrollState;

enum class PaintCommandCacheMode : u8 {
    ReadOnly,
    ReadWrite,
};

}

namespace Web {

class WEB_API DisplayListRecordingContext {
public:
    DisplayListRecordingContext(Palette const& palette, double device_pixels_per_css_pixel, ChromeMetrics const& chrome_metrics);

    Palette const& palette() const { return m_palette; }

    Painting::PaintCommandCacheMode paint_command_cache_mode() const { return m_paint_command_cache_mode; }
    void set_paint_command_cache_mode(Painting::PaintCommandCacheMode mode) { m_paint_command_cache_mode = mode; }

    DevicePixelRect device_viewport_rect() const { return m_device_viewport_rect; }
    void set_device_viewport_rect(DevicePixelRect const& rect) { m_device_viewport_rect = rect; }

    DevicePixels enclosing_device_pixels(CSSPixels css_pixels) const;
    DevicePixels floored_device_pixels(CSSPixels css_pixels) const;
    DevicePixels rounded_device_pixels(CSSPixels css_pixels) const;
    DevicePixelPoint rounded_device_point(CSSPixelPoint) const;
    DevicePixelPoint floored_device_point(CSSPixelPoint) const;
    DevicePixelRect enclosing_device_rect(CSSPixelRect) const;
    DevicePixelRect rounded_device_rect(CSSPixelRect) const;
    DevicePixelSize enclosing_device_size(CSSPixelSize) const;
    DevicePixelSize rounded_device_size(CSSPixelSize) const;

    Painting::DevicePixelConverter const& device_pixel_converter() const { return m_device_pixel_converter; }
    double device_pixels_per_css_pixel() const { return m_device_pixel_converter.device_pixels_per_css_pixel(); }
    ChromeMetrics const& chrome_metrics() const { return m_chrome_metrics; }
    u64 paint_generation_id() const { return m_paint_generation_id; }

    void set_async_scrolling_metadata_context(
        UniqueNodeID document_id,
        bool has_blocking_wheel_event_listeners,
        bool has_blocking_wheel_event_region_covering_viewport)
    {
        m_async_scrolling_document_id = document_id;
        m_is_recording_async_scrolling_metadata = true;
        m_has_blocking_wheel_event_listeners = has_blocking_wheel_event_listeners;
        m_has_blocking_wheel_event_region_covering_viewport = has_blocking_wheel_event_region_covering_viewport;
    }

    bool is_recording_async_scrolling_metadata() const { return m_is_recording_async_scrolling_metadata; }
    UniqueNodeID async_scrolling_document_id() const { return m_async_scrolling_document_id; }
    bool has_blocking_wheel_event_listeners() const { return m_has_blocking_wheel_event_listeners; }
    void set_has_blocking_wheel_event_listeners(bool value) { m_has_blocking_wheel_event_listeners = value; }
    bool has_blocking_wheel_event_region_covering_viewport() const { return m_has_blocking_wheel_event_region_covering_viewport; }

private:
    Palette m_palette;
    Painting::DevicePixelConverter m_device_pixel_converter;
    ChromeMetrics m_chrome_metrics;
    DevicePixelRect m_device_viewport_rect;
    Painting::PaintCommandCacheMode m_paint_command_cache_mode { Painting::PaintCommandCacheMode::ReadOnly };
    u64 m_paint_generation_id { 0 };
    UniqueNodeID m_async_scrolling_document_id {};
    bool m_is_recording_async_scrolling_metadata { false };
    bool m_has_blocking_wheel_event_listeners { false };
    bool m_has_blocking_wheel_event_region_covering_viewport { false };
};

}
