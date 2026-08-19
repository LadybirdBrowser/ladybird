/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGClipPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

NonnullRefPtr<SVGGraphicsPaintable> SVGGraphicsPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new SVGGraphicsPaintable(layout_box));
}

SVGGraphicsPaintable::SVGGraphicsPaintable(Layout::Box const& layout_box)
    : SVGPaintable(layout_box)
{
}

}
