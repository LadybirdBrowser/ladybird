/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGClipPathElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGClipPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGClipPathElement);

SVGClipPathElement::SVGClipPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGClipPathElement::~SVGClipPathElement()
{
}

void SVGClipPathElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGClipPathElement);
}

void SVGClipPathElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::clipPathUnits)
        m_clip_path_units = AttributeParser::parse_units(value.value_or(String {}));
}

GC::Ptr<Layout::Node> SVGClipPathElement::create_layout_node(GC::Ref<CSS::ComputedProperties>)
{
    // Clip paths are handled as a special case in the TreeBuilder.
    return nullptr;
}

}
