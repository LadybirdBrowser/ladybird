/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static Optional<SVG::SVGSVGElement::NaturalMetrics>
embedded_svg_metrics(HTML::NavigableContainer const& container)
{
    if (!is<HTML::HTMLObjectElement>(container))
        return {};

    if (auto const* content_document = container.content_document_without_origin_check()) {
        if (auto const* root = content_document->document_element();
            root && root->is_svg_svg_element()) {

            return SVG::SVGSVGElement::negotiate_natural_metrics(static_cast<SVG::SVGSVGElement const&>(*root));
        }
    }
    return {};
}

Optional<CSSPixels> NavigableContainerViewport::compute_natural_width() const
{
    if (auto metrics = embedded_svg_metrics(dom_node()); metrics.has_value()) {
        return metrics->width;
    }
    auto width_attr = dom_node().get_attribute_value(HTML::AttributeNames::width);
    return CSSPixels(width_attr.to_number<int>().value_or(300));
}

Optional<CSSPixels> NavigableContainerViewport::compute_natural_height() const
{
    if (auto metrics = embedded_svg_metrics(dom_node()); metrics.has_value()) {
        return metrics->height;
    }
    auto height_attr = dom_node().get_attribute_value(HTML::AttributeNames::height);
    return CSSPixels(height_attr.to_number<int>().value_or(150));
}

Optional<CSSPixelFraction> NavigableContainerViewport::compute_natural_aspect_ratio() const
{
    if (auto metrics = embedded_svg_metrics(dom_node()); metrics.has_value()) {
        return metrics->aspect_ratio;
    }

    return {};
}

void NavigableContainerViewport::did_set_content_size()
{
    ReplacedBox::did_set_content_size();

    if (dom_node().content_navigable())
        dom_node().content_navigable()->set_viewport_size(paintable_box()->content_size());
}

GC::Ptr<Painting::Paintable> NavigableContainerViewport::create_paintable() const
{
    return Painting::NavigableContainerViewportPaintable::create(*this);
}

}
