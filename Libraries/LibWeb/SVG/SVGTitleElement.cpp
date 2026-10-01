/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGTitleElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTitleElement);

SVGTitleElement::SVGTitleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

CSS::ElementBoxKind SVGTitleElement::box_kind() const
{
    return CSS::ElementBoxKind::NoBox;
}

void SVGTitleElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);

    auto navigable = document().navigable();
    if (!navigable || !navigable->is_top_level_traversable())
        return;

    auto* document_element = document().document_element();

    if (document_element == parent() && is<SVGElement>(document_element))
        document().page().client().page_did_change_title(document().title());
}

}
