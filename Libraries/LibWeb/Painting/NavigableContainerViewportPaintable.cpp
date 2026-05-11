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
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

NonnullRefPtr<NavigableContainerViewportPaintable> NavigableContainerViewportPaintable::create(Layout::NavigableContainerViewport const& layout_box)
{
    return adopt_ref(*new NavigableContainerViewportPaintable(layout_box));
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

        CSSPixelRect external_content_rect = absolute_rect;
        if (auto* root = as_if<SVG::SVGSVGElement>(hosted_document->document_element())) {
            // https://drafts.csswg.org/css-images-3/#the-object-fit
            // The object-fit property specifies how the contents of a replaced element should be fitted to the box established by its used height and width.
            // SVG uses the given size as the size of the SVG Viewport and then uses the values of several attributes on the root svg element to determine how to draw itself.
            external_content_rect = get_replaced_content_rect(*this, SVG::SVGSVGElement::negotiate_natural_metrics(*root));
        }

        context.display_list_recorder().save();
        context.display_list_recorder().add_clip_rect(clip_rect.to_type<int>());
        context.display_list_recorder().draw_external_content(
            context.rounded_device_rect(external_content_rect).to_type<int>(),
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
