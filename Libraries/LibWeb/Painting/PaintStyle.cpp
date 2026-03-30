/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

SVGPatternPaintStyle::SVGPatternPaintStyle(NonnullRefPtr<DisplayList> tile_display_list, Gfx::FloatRect tile_rect, Optional<Gfx::AffineTransform> pattern_transform)
    : tile_display_list(move(tile_display_list))
    , tile_rect(tile_rect)
    , pattern_transform(move(pattern_transform))
{
}

SVGPatternPaintStyle::~SVGPatternPaintStyle() = default;
SVGPatternPaintStyle::SVGPatternPaintStyle(SVGPatternPaintStyle const&) = default;
SVGPatternPaintStyle& SVGPatternPaintStyle::operator=(SVGPatternPaintStyle const&) = default;
SVGPatternPaintStyle::SVGPatternPaintStyle(SVGPatternPaintStyle&&) = default;
SVGPatternPaintStyle& SVGPatternPaintStyle::operator=(SVGPatternPaintStyle&&) = default;

}
