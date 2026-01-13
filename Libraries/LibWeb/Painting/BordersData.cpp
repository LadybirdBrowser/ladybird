/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BordersData.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web::Painting {

BordersDataDevicePixels BordersData::to_device_pixels(DisplayListRecordingContext const& context) const
{
    return BordersDataDevicePixels {
        BorderDataDevicePixels {
            top.color.resolved(),
            top.line_style,
            context.enclosing_device_pixels(top.width).value() },
        BorderDataDevicePixels {
            right.color.resolved(),
            right.line_style,
            context.enclosing_device_pixels(right.width).value() },
        BorderDataDevicePixels {
            bottom.color.resolved(),
            bottom.line_style,
            context.enclosing_device_pixels(bottom.width).value() },
        BorderDataDevicePixels {
            left.color.resolved(),
            left.line_style,
            context.enclosing_device_pixels(left.width).value() }
    };
}

}
