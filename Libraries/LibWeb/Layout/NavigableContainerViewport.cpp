/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(NavigableContainerViewport);

NavigableContainerViewport::NavigableContainerViewport(DOM::Document& document, HTML::NavigableContainer& element, CSS::StyleProperties style)
    : ReplacedBox(document, element, move(style))
{
}

NavigableContainerViewport::~NavigableContainerViewport() = default;

void NavigableContainerViewport::prepare_for_replaced_layout()
{
    if (is<HTML::HTMLObjectElement>(dom_node())) {
        if (auto const* content_document = dom_node().content_document_without_origin_check()) {
            if (auto const* root_element = content_document->document_element(); root_element && root_element->is_svg_svg_element()) {
                auto natural_metrics = SVG::SVGSVGElement::negotiate_natural_metrics(static_cast<SVG::SVGSVGElement const&>(*root_element));
                set_natural_width(natural_metrics.width);
                set_natural_height(natural_metrics.height);
                set_natural_aspect_ratio(natural_metrics.aspect_ratio);
                return;
            }
        }
    }
    // FIXME: Do proper error checking, etc.
    set_natural_width(dom_node().get_attribute_value(HTML::AttributeNames::width).to_number<int>().value_or(300));
    set_natural_height(dom_node().get_attribute_value(HTML::AttributeNames::height).to_number<int>().value_or(150));
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
