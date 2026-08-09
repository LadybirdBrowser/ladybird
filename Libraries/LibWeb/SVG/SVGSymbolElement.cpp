/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGSymbolElement.h>
#include <LibWeb/SVG/SVGUseElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGSymbolElement);

SVGSymbolElement::SVGSymbolElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, qualified_name)
{
}

void SVGSymbolElement::initialize_element()
{
    SVGFitToViewBox::initialize_fit_to_view_box();
}

void SVGSymbolElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
}

void SVGSymbolElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);
}

bool SVGSymbolElement::is_direct_child_of_use_shadow_tree() const
{
    auto maybe_shadow_root = parent();
    if (!is<DOM::ShadowRoot>(maybe_shadow_root)) {
        return false;
    }

    auto host = static_cast<DOM::ShadowRoot const&>(*maybe_shadow_root).host();
    return is<SVGUseElement>(host);
}

RefPtr<Layout::Node> SVGSymbolElement::create_layout_node(CSS::LayoutStyle style)
{
    // https://svgwg.org/svg2-draft/render.html#TermNeverRenderedElement
    // [..] it also includes a ‘symbol’ element that is not the instance root of a use-element shadow tree.
    if (!is_direct_child_of_use_shadow_tree())
        return {};

    return make_ref_counted<Layout::SVGGraphicsBox>(document(), *this, style);
}

}
