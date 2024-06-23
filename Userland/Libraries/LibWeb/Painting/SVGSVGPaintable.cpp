/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(SVGSVGPaintable);

JS::NonnullGCPtr<SVGSVGPaintable> SVGSVGPaintable::create(Layout::SVGSVGBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<SVGSVGPaintable>(layout_box);
}

SVGSVGPaintable::SVGSVGPaintable(Layout::SVGSVGBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::SVGSVGBox const& SVGSVGPaintable::layout_box() const
{
    return static_cast<Layout::SVGSVGBox const&>(layout_node());
}

void SVGSVGPaintable::before_children_paint(PaintContext& context, PaintPhase phase) const
{
    PaintableBox::before_children_paint(context, phase);
    if (phase != PaintPhase::Foreground)
        return;
    context.display_list_recorder().save();
    auto clip_rect = absolute_rect();
    clip_rect.translate_by(enclosing_scroll_frame_offset().value_or({}));
    context.display_list_recorder().add_clip_rect(context.enclosing_device_rect(clip_rect).to_type<int>());
}

void SVGSVGPaintable::after_children_paint(PaintContext& context, PaintPhase phase) const
{
    PaintableBox::after_children_paint(context, phase);
    if (phase != PaintPhase::Foreground)
        return;
    context.display_list_recorder().restore();
}

}
