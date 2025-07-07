/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

SVGPaintable::SVGPaintable(Layout::SVGBox const& layout_box)
    : PaintableBox(layout_box)
{
}

void SVGPaintable::before_paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;
    if (!has_css_transform()) {
        apply_clip_overflow_rect(context, phase);
    }
    apply_scroll_offset(context);
}

void SVGPaintable::after_paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;
    reset_scroll_offset(context);
    if (!has_css_transform()) {
        clear_clip_overflow_rect(context, phase);
    }
}

Layout::SVGBox const& SVGPaintable::layout_box() const
{
    return static_cast<Layout::SVGBox const&>(layout_node());
}

CSSPixelRect SVGPaintable::compute_absolute_rect() const
{
    if (auto* svg_svg_box = layout_box().first_ancestor_of_type<Layout::SVGSVGBox>()) {
        CSSPixelRect rect { offset(), content_size() };
        for (Layout::Box const* ancestor = svg_svg_box; ancestor; ancestor = ancestor->containing_block())
            rect.translate_by(ancestor->paintable_box()->offset());
        return rect;
    }
    return PaintableBox::compute_absolute_rect();
}

}
