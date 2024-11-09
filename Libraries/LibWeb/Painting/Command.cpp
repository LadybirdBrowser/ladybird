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
    auto shadow_rect = box_shadow_params.device_content_rect;
    auto spread = box_shadow_params.blur_radius * 2 + box_shadow_params.spread_distance;
    shadow_rect.inflate(spread, spread, spread, spread);
    auto offset_x = box_shadow_params.offset_x;
    auto offset_y = box_shadow_params.offset_y;
    shadow_rect.translate_by(offset_x, offset_y);
    return shadow_rect;
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
