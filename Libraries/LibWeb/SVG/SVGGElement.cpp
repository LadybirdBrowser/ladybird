/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGGElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGGElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGGElement);

SVGGElement::SVGGElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGGElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGGElement);
    Base::initialize(realm);
}

RefPtr<Layout::Node> SVGGElement::create_layout_node(CSS::ComputedProperties const& style)
{
    return make_ref_counted<Layout::SVGGraphicsBox>(document(), *this, style);
}

}
