/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MathMLAnchorElement.h>
#include <LibWeb/MathML/AttributeNames.h>
#include <LibWeb/MathML/MathMLAnchorElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLAnchorElement);

MathMLAnchorElement::MathMLAnchorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

MathMLAnchorElement::~MathMLAnchorElement() = default;

void MathMLAnchorElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::href && !namespace_.has_value())
        href_content_attribute_changed();
}

}
