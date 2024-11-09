/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Painting/SVGClipPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

JS::NonnullGCPtr<SVGGraphicsPaintable> SVGGraphicsPaintable::create(Layout::SVGGraphicsBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<SVGGraphicsPaintable>(layout_box);
}

SVGGraphicsPaintable::SVGGraphicsPaintable(Layout::SVGGraphicsBox const& layout_box)
    : SVGPaintable(layout_box)
{
}

Layout::SVGGraphicsBox const& SVGGraphicsPaintable::layout_box() const
{
    return static_cast<Layout::SVGGraphicsBox const&>(layout_node());
}

}
