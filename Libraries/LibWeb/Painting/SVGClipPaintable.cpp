/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGClipPaintable.h>

namespace Web::Painting {

NonnullRefPtr<SVGClipPaintable> SVGClipPaintable::create(Layout::SVGClipBox const& layout_box)
{
    return adopt_ref(*new SVGClipPaintable(layout_box));
}

SVGClipPaintable::SVGClipPaintable(Layout::SVGClipBox const& layout_box)
    : SVGPaintable(layout_box)
{
}

}
