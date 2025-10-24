/*
 * Copyright (c) 2025, Lorenz Ackermann, <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/MathML/AttributeNames.h>
#include <LibWeb/MathML/MathMLMiElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLMiElement);

MathMLMiElement::MathMLMiElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

bool MathMLMiElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;
    return first_is_one_of(name, AttributeNames::mathvariant);
}

void MathMLMiElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);
    // https://w3c.github.io/mathml-core/#dfn-mathvariant
    // The mathvariant attribute, if present, must be an ASCII case-insensitive match of normal. In that case, the user
    // agent is expected to treat the attribute as a presentational hint setting the element's text-transform property
    // to none. Otherwise it has no effects.
    if (auto mathvariant = attribute(AttributeNames::mathvariant); mathvariant.has_value() && mathvariant.value().equals_ignoring_ascii_case("normal"sv)) {
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextTransform, CSS::KeywordStyleValue::create(CSS::Keyword::None));
    }
}

}
