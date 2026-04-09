/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibGfx/Path.h>
#include <LibWeb/Bindings/SVGPathElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/SVG/SVGPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPathElement);

SVGPathElement::SVGPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, move(qualified_name))
{
}

void SVGPathElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGPathElement);
    Base::initialize(realm);
}

void SVGPathElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == "d") {
        m_path = AttributeParser::parse_path_data(value.value_or(String {}));
        set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
    }
}

Gfx::Path SVGPathElement::get_path(CSSPixelSize)
{
    return m_path.to_gfx_path();
}

}
