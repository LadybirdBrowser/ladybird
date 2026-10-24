/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGMarkerElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGMarkerElement);

SVGMarkerElement::SVGMarkerElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGMarkerElement::initialize_element()
{
    SVGFitToViewBox::initialize_fit_to_view_box(*this);
}

void SVGMarkerElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFitToViewBox::visit_edges(visitor);
    visitor.visit(m_orient_type);
    visitor.visit(m_orient_angle);
}

void SVGMarkerElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGFitToViewBox::attribute_changed(*this, name, value);
}

// https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__markerUnits
GC::Ref<SVGAnimatedEnumeration> SVGMarkerElement::marker_units()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "userSpaceOnUse"sv, SVG_MARKERUNITS_USERSPACEONUSE },
        SVGAnimatedEnumeration::Keyword { "strokeWidth"sv, SVG_MARKERUNITS_STROKEWIDTH },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::markerUnits, keywords, SVG_MARKERUNITS_STROKEWIDTH);
}

// https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__orientType
GC::Ref<SVGAnimatedEnumeration> SVGMarkerElement::orient_type()
{
    // The numeric type values for ‘orient’ are as follows:
    // auto: SVG_MARKER_ORIENT_AUTO
    // auto-start-reverse: SVG_MARKER_ORIENT_AUTO_START_REVERSE
    // <angle> | <number>: SVG_MARKER_ORIENT_ANGLE
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "auto"sv, SVG_MARKER_ORIENT_AUTO },
        SVGAnimatedEnumeration::Keyword { "auto-start-reverse"sv, SVG_MARKER_ORIENT_AUTO_START_REVERSE },
    };
    if (!m_orient_type)
        m_orient_type = SVGAnimatedEnumeration::create(*this, AttributeNames::orient, keywords, SVG_MARKER_ORIENT_ANGLE, SVGAnimatedEnumeration::ReflectsOrientType::Yes);
    return *m_orient_type;
}

// https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__orientAngle
GC::Ref<SVGAnimatedAngle> SVGMarkerElement::orient_angle()
{
    if (!m_orient_angle) {
        auto base_val = SVGAngle::create_reflected_attribute(*this, AttributeNames::orient, SVGAngle::ReadOnly::No);
        auto anim_val = SVGAngle::create_reflected_attribute(*this, AttributeNames::orient, SVGAngle::ReadOnly::Yes);
        m_orient_angle = SVGAnimatedAngle::create(base_val, anim_val);
    }
    return *m_orient_angle;
}

// https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__setOrientToAuto
void SVGMarkerElement::set_orient_to_auto()
{
    // When setOrientToAuto() is called, the ‘orient’ attribute is simply set to 'auto'.
    set_attribute_value(AttributeNames::orient, "auto"_utf16);
}

// https://svgwg.org/svg2-draft/painting.html#__svg__SVGMarkerElement__setOrientToAngle
void SVGMarkerElement::set_orient_to_angle(GC::Ref<SVGAngle> angle)
{
    // When setOrientToAngle(angle) is called, the ‘orient’ attribute is reserialized using angle as the value.
    set_attribute_value(AttributeNames::orient, angle->value_as_string());
}

}
