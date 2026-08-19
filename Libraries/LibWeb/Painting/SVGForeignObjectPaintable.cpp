/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGForeignObjectPaintable.h>

namespace Web::Painting {

NonnullRefPtr<SVGForeignObjectPaintable> SVGForeignObjectPaintable::create(Layout::BlockContainer const& layout_box)
{
    return adopt_ref(*new SVGForeignObjectPaintable(layout_box));
}

SVGForeignObjectPaintable::SVGForeignObjectPaintable(Layout::BlockContainer const& layout_box)
    : PaintableWithLines(layout_box)
{
}

}
