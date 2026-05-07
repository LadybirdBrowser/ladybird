/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGPatternPaintable.h>

namespace Web::Painting {

NonnullRefPtr<SVGPatternPaintable> SVGPatternPaintable::create(Layout::SVGPatternBox const& layout_box)
{
    return adopt_ref(*new SVGPatternPaintable(layout_box));
}

SVGPatternPaintable::SVGPatternPaintable(Layout::SVGPatternBox const& layout_box)
    : SVGPaintable(layout_box)
{
}

}
