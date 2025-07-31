/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web::Painting {

CornerRadius BorderRadiusData::as_corner(DevicePixelConverter const& device_pixel_scale) const
{
    return CornerRadius {
        device_pixel_scale.floored_device_pixels(horizontal_radius).value(),
        device_pixel_scale.floored_device_pixels(vertical_radius).value()
    };
}

}
