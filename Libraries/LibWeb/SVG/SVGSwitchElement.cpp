/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGSwitchElement.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGSwitchElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGSwitchElement);

SVGSwitchElement::SVGSwitchElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGSwitchElement::~SVGSwitchElement() = default;

void SVGSwitchElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGSwitchElement);
    Base::initialize(realm);
}

RefPtr<Layout::Node> SVGSwitchElement::create_layout_node(CSS::ComputedProperties const& style)
{
    return make_ref_counted<Layout::SVGGraphicsBox>(document(), *this, style);
}

}
