/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/Command.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

void DrawGlyphRun::translate_by(Gfx::IntPoint const& offset)
{
    rect.translate_by(offset);
    translation.translate_by(offset.to_type<float>());
}

Gfx::IntRect PaintOuterBoxShadow::bounding_rect() const
{
    return get_outer_box_shadow_bounding_rect(box_shadow_params);
}

Gfx::IntRect PaintInnerBoxShadow::bounding_rect() const
{
    return box_shadow_params.device_content_rect;
}

void PaintOuterBoxShadow::translate_by(Gfx::IntPoint const& offset)
{
    box_shadow_params.device_content_rect.translate_by(offset);
}

void PaintInnerBoxShadow::translate_by(Gfx::IntPoint const& offset)
{
    box_shadow_params.device_content_rect.translate_by(offset);
}

}
