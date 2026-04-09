/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGPatternPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(SVGPatternPaintable);

GC::Ref<SVGPatternPaintable> SVGPatternPaintable::create(Layout::SVGPatternBox const& layout_box)
{
    return layout_box.heap().allocate<SVGPatternPaintable>(layout_box);
}

SVGPatternPaintable::SVGPatternPaintable(Layout::SVGPatternBox const& layout_box)
    : SVGPaintable(layout_box)
{
}

}
