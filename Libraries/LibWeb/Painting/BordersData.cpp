/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BordersData.h>
#include <LibWeb/Painting/PaintContext.h>

namespace Web::Painting {

BordersDataDevicePixels BordersData::to_device_pixels(PaintContext const& context) const
{
    return BordersDataDevicePixels {
        BorderDataDevicePixels {
            top.color,
            top.line_style,
            context.enclosing_device_pixels(top.width).value() },
        BorderDataDevicePixels {
            right.color,
            right.line_style,
            context.enclosing_device_pixels(right.width).value() },
        BorderDataDevicePixels {
            bottom.color,
            bottom.line_style,
            context.enclosing_device_pixels(bottom.width).value() },
        BorderDataDevicePixels {
            left.color,
            left.line_style,
            context.enclosing_device_pixels(left.width).value() }
    };
}

}
