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

// https://drafts.csswg.org/css-backgrounds/#shadow-shape
static void add_spread_distance_to_border_radius(int& border_radius, int spread_distance)
{
    if (border_radius == 0 || spread_distance == 0)
        return;

    // To preserve the box's shape when spread is applied, the corner radii of the shadow are also increased (decreased,
    // for inner shadows) from the border-box (padding-box) radii by adding (subtracting) the spread distance (and flooring
    // at zero). However, in order to create a sharper corner when the border radius is small (and thus ensure continuity
    // between round and sharp corners), when the border radius is less than the spread distance (or in the case of an inner
    // shadow, less than the absolute value of a negative spread distance), the spread distance is first multiplied by the
    // proportion 1 + (r-1)^3, where r is the ratio of the border radius to the spread distance, in calculating the corner
    // radii of the spread shadow shape.
    if (border_radius > AK::abs(spread_distance)) {
        border_radius += spread_distance;
    } else {
        auto r = (float)border_radius / AK::abs(spread_distance);
        border_radius += spread_distance * (1 + AK::pow(r - 1, 3.0f));
    }
}

void CornerRadii::adjust_corners_for_spread_distance(int spread_distance)
{
    add_spread_distance_to_border_radius(top_left.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(top_left.vertical_radius, spread_distance);
    add_spread_distance_to_border_radius(top_right.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(top_right.vertical_radius, spread_distance);
    add_spread_distance_to_border_radius(bottom_right.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(bottom_right.vertical_radius, spread_distance);
    add_spread_distance_to_border_radius(bottom_left.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(bottom_left.vertical_radius, spread_distance);
}

}
