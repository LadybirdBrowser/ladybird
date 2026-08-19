/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGImagePaintable.h>

namespace Web::Painting {

NonnullRefPtr<SVGImagePaintable> SVGImagePaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new SVGImagePaintable(layout_box));
}

SVGImagePaintable::SVGImagePaintable(Layout::Box const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

}
