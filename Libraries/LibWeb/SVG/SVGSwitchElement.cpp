/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGSwitchElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/SVG/SVGSwitchElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGSwitchElement);

SVGSwitchElement::SVGSwitchElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGSwitchElement::~SVGSwitchElement() = default;

RefPtr<Layout::Node> SVGSwitchElement::create_layout_node(CSS::LayoutStyle style)
{
    return make_ref_counted<Layout::Box>(document(), *this, style, Layout::RustFFI::NodeKind::SVGGraphicsBox);
}

}
