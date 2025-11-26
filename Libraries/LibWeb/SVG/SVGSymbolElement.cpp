/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGSymbolElementPrototype.h>
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

void SVGSymbolElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGSymbolElement);
    Base::initialize(realm);
    SVGFitToViewBox::initialize(realm);
}

void SVGSymbolElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
}

void SVGSymbolElement::adjust_computed_style(CSS::ComputedProperties& computed_properties)
{
    if (is_direct_child_of_use_shadow_tree()) {
        // The generated instance of a ‘symbol’ that is the direct referenced element of a ‘use’ element must always have a computed value of inline for the display property.
        computed_properties.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Inline)));
    }
}

void SVGSymbolElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
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

GC::Ptr<Layout::Node> SVGSymbolElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    // https://svgwg.org/svg2-draft/render.html#TermNeverRenderedElement
    // [..] it also includes a ‘symbol’ element that is not the instance root of a use-element shadow tree.
    if (!is_direct_child_of_use_shadow_tree())
        return {};

    return heap().allocate<Layout::SVGGraphicsBox>(document(), *this, style);
}

}
