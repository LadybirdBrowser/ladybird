/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

struct BorderDataDevicePixels {
public:
    Color color { Color::Transparent };
    CSS::LineStyle line_style { CSS::LineStyle::None };
    DevicePixels width { 0 };
};

struct BordersDataDevicePixels {
    BorderDataDevicePixels top;
    BorderDataDevicePixels right;
    BorderDataDevicePixels bottom;
    BorderDataDevicePixels left;
};

struct BordersData {
    CSS::BorderData top;
    CSS::BorderData right;
    CSS::BorderData bottom;
    CSS::BorderData left;

    BordersDataDevicePixels to_device_pixels(PaintContext const& context) const;
};

}
