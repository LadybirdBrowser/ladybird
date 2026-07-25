/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGForeignObjectElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/SVGForeignObjectBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGForeignObjectElement);

SVGForeignObjectElement::SVGForeignObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGForeignObjectElement::~SVGForeignObjectElement() = default;

void SVGForeignObjectElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGForeignObjectElement);
    Base::initialize(realm);
}

RefPtr<Layout::Node> SVGForeignObjectElement::create_layout_node(NonnullRefPtr<CSS::ComputedValues const> style)
{
    return make_ref_counted<Layout::SVGForeignObjectBox>(document(), *this, style);
}

}
