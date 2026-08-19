/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Painting {

NonnullRefPtr<SVGSVGPaintable> SVGSVGPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new SVGSVGPaintable(layout_box));
}

SVGSVGPaintable::SVGSVGPaintable(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

}
