/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGClipPaintable.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(SVGClipPaintable);

JS::NonnullGCPtr<SVGClipPaintable> SVGClipPaintable::create(Layout::SVGClipBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<SVGClipPaintable>(layout_box);
}

SVGClipPaintable::SVGClipPaintable(Layout::SVGClipBox const& layout_box)
    : SVGPaintable(layout_box)
{
}

}
