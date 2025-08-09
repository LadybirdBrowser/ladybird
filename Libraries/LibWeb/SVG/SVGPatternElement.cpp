/*
 * Copyright (c) 2025, Ankit Khandelwal <ankk98@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGPatternElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>

namespace Web::SVG {

SVGPatternElement::SVGPatternElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGPatternElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGPatternElement);
    Base::initialize(realm);
}

void SVGPatternElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    auto const& v = value.value_or(String {});
    if (name == AttributeNames::x) {
        m_x = AttributeParser::parse_coordinate(v);
    } else if (name == AttributeNames::y) {
        m_y = AttributeParser::parse_coordinate(v);
    } else if (name == AttributeNames::width) {
        m_width = AttributeParser::parse_positive_length(v);
    } else if (name == AttributeNames::height) {
        m_height = AttributeParser::parse_positive_length(v);
    } else if (name == AttributeNames::patternUnits) {
        m_pattern_units = AttributeParser::parse_units(v);
    } else if (name == AttributeNames::patternContentUnits) {
        m_pattern_content_units = AttributeParser::parse_units(v);
    } else if (name == AttributeNames::patternTransform) {
        if (auto list = AttributeParser::parse_transform(v); list.has_value())
            m_pattern_transform = transform_from_transform_list(*list);
        else
            m_pattern_transform = {};
    }
}

}
