/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Painting/BorderPainting.h>

namespace Web::Painting {

enum class CornerClip {
    Outside,
    Inside
};

struct WEB_API ScopedCornerRadiusClip {
    ScopedCornerRadiusClip(DisplayListRecordingContext& context, DevicePixelRect const& border_rect, BorderRadiiData const& border_radii, CornerClip corner_clip = CornerClip::Outside, bool do_apply = true);

    ~ScopedCornerRadiusClip();

    AK_MAKE_NONMOVABLE(ScopedCornerRadiusClip);
    AK_MAKE_NONCOPYABLE(ScopedCornerRadiusClip);

private:
    DisplayListRecordingContext& m_context;
    bool m_has_radius { false };
    bool m_do_apply;
};

}
