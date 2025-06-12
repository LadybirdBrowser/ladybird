/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGClipPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(SVGClipPaintable);

GC::Ref<SVGClipPaintable> SVGClipPaintable::create(Layout::SVGClipBox const& layout_box)
{
    return layout_box.heap().allocate<SVGClipPaintable>(layout_box);
}

SVGClipPaintable::SVGClipPaintable(Layout::SVGClipBox const& layout_box)
    : SVGPaintable(layout_box)
{
}

}
