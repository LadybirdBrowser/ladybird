/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(NavigableContainerViewport);

NavigableContainerViewport::NavigableContainerViewport(DOM::Document& document, HTML::NavigableContainer& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, move(style))
{
}

NavigableContainerViewport::~NavigableContainerViewport() = default;

CSS::SizeWithAspectRatio NavigableContainerViewport::natural_size() const
{
    if (!is<HTML::HTMLObjectElement>(dom_node()))
        return {};

    if (auto const* content_document = dom_node().content_document_without_origin_check()) {
        if (auto const* root = content_document->document_element();
            root && root->is_svg_svg_element()) {

            auto metrics = SVG::SVGSVGElement::negotiate_natural_metrics(static_cast<SVG::SVGSVGElement const&>(*root));
            return { metrics.width, metrics.height, metrics.aspect_ratio };
        }
    }
    return {};
}

void NavigableContainerViewport::did_set_content_size()
{
    ReplacedBox::did_set_content_size();

    if (auto content_navigable = dom_node().content_navigable()) {
        CSSPixelSize viewport_size = paintable_box()->content_size();
        CSS::SizeWithAspectRatio const natural_size = this->natural_size();
        if (natural_size.has_width() || natural_size.has_height() || natural_size.has_aspect_ratio())
            viewport_size = CSS::replaced_object_fit_size(computed_values().object_fit(), natural_size, viewport_size);
        content_navigable->set_viewport_size(viewport_size);
    }
}

GC::Ptr<Painting::Paintable> NavigableContainerViewport::create_paintable() const
{
    return Painting::NavigableContainerViewportPaintable::create(*this);
}

}
