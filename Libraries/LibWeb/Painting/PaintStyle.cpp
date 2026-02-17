/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

NonnullRefPtr<SVGPatternPaintStyle> SVGPatternPaintStyle::create(NonnullRefPtr<DisplayList> tile_display_list, Gfx::FloatRect tile_rect, Optional<Gfx::AffineTransform> pattern_transform)
{
    return adopt_ref(*new SVGPatternPaintStyle(move(tile_display_list), tile_rect, move(pattern_transform)));
}

SVGPatternPaintStyle::SVGPatternPaintStyle(NonnullRefPtr<DisplayList> tile_display_list, Gfx::FloatRect tile_rect, Optional<Gfx::AffineTransform> pattern_transform)
    : m_tile_display_list(move(tile_display_list))
    , m_tile_rect(tile_rect)
    , m_pattern_transform(move(pattern_transform))
{
}

SVGPatternPaintStyle::~SVGPatternPaintStyle() = default;

}
