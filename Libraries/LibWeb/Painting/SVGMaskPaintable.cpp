/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGMaskPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(SVGMaskPaintable);

GC::Ref<SVGMaskPaintable> SVGMaskPaintable::create(Layout::SVGMaskBox const& layout_box)
{
    return layout_box.heap().allocate<SVGMaskPaintable>(layout_box);
}

SVGMaskPaintable::SVGMaskPaintable(Layout::SVGMaskBox const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

}
