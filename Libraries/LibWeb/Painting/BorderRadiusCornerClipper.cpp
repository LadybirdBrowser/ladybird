/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web::Painting {

ScopedCornerRadiusClip::ScopedCornerRadiusClip(DisplayListRecordingContext& context, DevicePixelRect const& border_rect, BorderRadiiData const& border_radii, CornerClip corner_clip, bool do_apply)
    : m_context(context)
{
    m_do_apply = do_apply;
    if (!do_apply)
        return;
    CornerRadii const corner_radii {
        .top_left = border_radii.top_left.as_corner(context.device_pixel_converter()),
        .top_right = border_radii.top_right.as_corner(context.device_pixel_converter()),
        .bottom_right = border_radii.bottom_right.as_corner(context.device_pixel_converter()),
        .bottom_left = border_radii.bottom_left.as_corner(context.device_pixel_converter())
    };
    m_has_radius = corner_radii.has_any_radius();
    if (!m_has_radius)
        return;
    m_context.display_list_recorder().save();
    m_context.display_list_recorder().add_rounded_rect_clip(corner_radii, border_rect.to_type<int>(), corner_clip);
}

ScopedCornerRadiusClip::~ScopedCornerRadiusClip()
{
    if (!m_do_apply || !m_has_radius)
        return;
    m_context.display_list_recorder().restore();
}

}
