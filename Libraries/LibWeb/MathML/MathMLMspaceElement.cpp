/*
 * Copyright (c) 2025, Lorenz Ackermann, <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/MathML/AttributeNames.h>
#include <LibWeb/MathML/MathMLMspaceElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLMspaceElement);

MathMLMspaceElement::MathMLMspaceElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

bool MathMLMspaceElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;
    return first_is_one_of(name, AttributeNames::width, AttributeNames::height, AttributeNames::depth);
}

void MathMLMspaceElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);
    // https://w3c.github.io/mathml-core/#attribute-mspace-width
    // The width, height, depth, if present, must have a value that is a valid <length-percentage>.
    auto parse_non_percentage_value = [&](FlyString const& attribute_name) -> RefPtr<CSS::StyleValue const> {
        if (auto attribute = this->attribute(attribute_name); attribute.has_value()) {
            if (auto value = HTML::parse_dimension_value(attribute.value()); value && !value->is_percentage()) {
                return value;
            }
        }
        return nullptr;
    };

    // If the width attribute is present, valid and not a percentage then that attribute is used as a presentational hint
    // setting the element's width property to the corresponding value.
    if (auto width_value = parse_non_percentage_value(AttributeNames::width)) {
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, width_value.release_nonnull());
    }

    // https://w3c.github.io/mathml-core/#attribute-mspace-height
    // If the height attribute is absent, invalid or a percentage then the requested line-ascent is 0. Otherwise the
    // requested line-ascent is the resolved value of the height attribute, clamping negative values to 0.
    auto height_value = parse_non_percentage_value(AttributeNames::height);
    // Note: line-ascent handling is managed by the layout system

    // If both the height and depth attributes are present, valid and not a percentage then they are used as a
    // presentational hint setting the element's height property to the concatenation of the
    // strings "calc(", the height attribute value, " + ", the depth attribute value, and ")". If only one of these
    // attributes is present, valid and not a percentage then it is treated as a presentational hint setting the
    // element's height property to the corresponding value.
    auto depth_value = parse_non_percentage_value(AttributeNames::depth);

    if (height_value && depth_value) {
        // Create calc(height + depth) as a CSS string
        auto height_str = attribute(AttributeNames::height).value();
        auto depth_str = attribute(AttributeNames::depth).value();
        auto calc_string = MUST(String::formatted("calc({} + {})", height_str, depth_str));
        if (auto calc_value = parse_css_value(CSS::Parser::ParsingParams { document() }, calc_string, CSS::PropertyID::Height)) {
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, calc_value.release_nonnull());
        }
    } else if (height_value) {
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, height_value.release_nonnull());
    } else if (depth_value) {
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, depth_value.release_nonnull());
    }
}

}
