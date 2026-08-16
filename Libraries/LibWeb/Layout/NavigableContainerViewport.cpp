/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

NavigableContainerViewport::NavigableContainerViewport(DOM::Document& document, HTML::NavigableContainer& element, CSS::LayoutStyle style)
    : ReplacedBox(document, element, style)
{
}

NavigableContainerViewport::~NavigableContainerViewport() = default;

void NavigableContainerViewport::did_set_content_size()
{
    ReplacedBox::did_set_content_size();

    if (auto content_navigable = dom_node().content_navigable()) {
        auto content_size = paintable_box()->content_size();
        as<HTML::LocalNavigable>(*content_navigable).set_viewport_size(content_size);
        document().page().client().page_did_update_child_frame_viewport(content_navigable->id(), paintable_box()->absolute_rect());
    }
}

}
