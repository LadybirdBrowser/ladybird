/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/BorderPainting.h>

namespace Web::Painting {

enum class CornerClip {
    Outside,
    Inside
};

struct ScopedCornerRadiusClip {
    ScopedCornerRadiusClip(PaintContext& context, DevicePixelRect const& border_rect, BorderRadiiData const& border_radii, CornerClip corner_clip = CornerClip::Outside);

    ~ScopedCornerRadiusClip();

    AK_MAKE_NONMOVABLE(ScopedCornerRadiusClip);
    AK_MAKE_NONCOPYABLE(ScopedCornerRadiusClip);

private:
    PaintContext& m_context;
    bool m_has_radius { false };
};

}
