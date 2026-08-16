/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGMaskPaintable.h>

namespace Web::Painting {

NonnullRefPtr<SVGMaskPaintable> SVGMaskPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new SVGMaskPaintable(layout_box));
}

SVGMaskPaintable::SVGMaskPaintable(Layout::Box const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

}
