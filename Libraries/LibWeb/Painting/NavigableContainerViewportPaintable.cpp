/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(NavigableContainerViewportPaintable);

GC::Ref<NavigableContainerViewportPaintable> NavigableContainerViewportPaintable::create(Layout::NavigableContainerViewport const& layout_box)
{
    return layout_box.heap().allocate<NavigableContainerViewportPaintable>(layout_box);
}

NavigableContainerViewportPaintable::NavigableContainerViewportPaintable(Layout::NavigableContainerViewport const& layout_box)
    : PaintableBox(layout_box)
{
}

void NavigableContainerViewportPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto absolute_rect = this->absolute_rect();
        auto clip_rect = context.rounded_device_rect(absolute_rect);
        ScopedCornerRadiusClip corner_clip { context, clip_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

        auto const& navigable_container = this->navigable_container();
        auto* hosted_document = const_cast<DOM::Document*>(navigable_container.content_document_without_origin_check());
        if (!hosted_document)
            return;

        if (hosted_document->is_render_blocked())
            return;

        auto content_navigable = navigable_container.content_navigable();
        VERIFY(content_navigable);

        context.display_list_recorder().save();
        context.display_list_recorder().add_clip_rect(clip_rect.to_type<int>());
        context.display_list_recorder().draw_external_content(
            context.enclosing_device_rect(absolute_rect).to_type<int>(),
            content_navigable->external_content_source(),
            Gfx::ScalingMode::NearestNeighbor);
        context.display_list_recorder().restore();

        if constexpr (HIGHLIGHT_FOCUSED_FRAME_DEBUG) {
            if (content_navigable->is_focused()) {
                context.display_list_recorder().draw_rect(clip_rect.to_type<int>(), Color::Cyan);
            }
        }
    }
}

}
