/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGMetadataElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGMetadataElement);

SVGMetadataElement::SVGMetadataElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

RefPtr<Layout::Node> SVGMetadataElement::create_layout_node(CSS::ComputedProperties const&)
{
    return nullptr;
}

}
