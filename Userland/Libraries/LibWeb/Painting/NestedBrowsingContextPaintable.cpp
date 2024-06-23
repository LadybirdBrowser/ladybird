/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/FrameBox.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/NestedBrowsingContextPaintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(NestedBrowsingContextPaintable);

JS::NonnullGCPtr<NestedBrowsingContextPaintable> NestedBrowsingContextPaintable::create(Layout::FrameBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<NestedBrowsingContextPaintable>(layout_box);
}

NestedBrowsingContextPaintable::NestedBrowsingContextPaintable(Layout::FrameBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::FrameBox const& NestedBrowsingContextPaintable::layout_box() const
{
    return static_cast<Layout::FrameBox const&>(layout_node());
}

void NestedBrowsingContextPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto absolute_rect = this->absolute_rect();
        auto clip_rect = context.rounded_device_rect(absolute_rect);
        ScopedCornerRadiusClip corner_clip { context, clip_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

        auto const* hosted_document = layout_box().dom_node().content_document_without_origin_check();
        if (!hosted_document)
            return;
        auto const* hosted_paint_tree = hosted_document->paintable();
        if (!hosted_paint_tree)
            return;

        context.display_list_recorder().save();

        context.display_list_recorder().add_clip_rect(clip_rect.to_type<int>());
        auto absolute_device_rect = context.enclosing_device_rect(absolute_rect);
        context.display_list_recorder().translate(absolute_device_rect.x().value(), absolute_device_rect.y().value());

        HTML::Navigable::PaintConfig paint_config;
        paint_config.paint_overlay = context.should_paint_overlay();
        paint_config.should_show_line_box_borders = context.should_show_line_box_borders();
        paint_config.has_focus = context.has_focus();
        const_cast<DOM::Document*>(hosted_document)->navigable()->record_display_list(context.display_list_recorder(), paint_config);

        context.display_list_recorder().restore();

        if constexpr (HIGHLIGHT_FOCUSED_FRAME_DEBUG) {
            if (layout_box().dom_node().content_navigable()->is_focused()) {
                context.display_list_recorder().draw_rect(clip_rect.to_type<int>(), Color::Cyan);
            }
        }
    }
}

}
