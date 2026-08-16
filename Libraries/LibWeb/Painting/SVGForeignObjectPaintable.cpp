/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/SVGForeignObjectPaintable.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

NonnullRefPtr<SVGForeignObjectPaintable> SVGForeignObjectPaintable::create(Layout::BlockContainer const& layout_box)
{
    return adopt_ref(*new SVGForeignObjectPaintable(layout_box));
}

SVGForeignObjectPaintable::SVGForeignObjectPaintable(Layout::BlockContainer const& layout_box)
    : PaintableWithLines(layout_box)
{
}

void SVGForeignObjectPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableWithLines::paint(context, phase);
}

}
