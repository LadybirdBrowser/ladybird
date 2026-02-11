/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFETurbulenceElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFETurbulenceElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFETurbulenceElement);

SVGFETurbulenceElement::SVGFETurbulenceElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFETurbulenceElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFETurbulenceElement);
    Base::initialize(realm);
}

void SVGFETurbulenceElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_base_frequency_x);
    visitor.visit(m_base_frequency_y);
    visitor.visit(m_num_octaves);
    visitor.visit(m_seed);
    visitor.visit(m_stitch_tiles);
    visitor.visit(m_type);
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-basefrequencyx
GC::Ref<SVGAnimatedNumber> SVGFETurbulenceElement::base_frequency_x()
{
    if (!m_base_frequency_x) {
        m_base_frequency_x = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::baseFrequency, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    }
    return *m_base_frequency_x;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-basefrequencyy
GC::Ref<SVGAnimatedNumber> SVGFETurbulenceElement::base_frequency_y()
{
    if (!m_base_frequency_y) {
        m_base_frequency_y = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::baseFrequency, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    }
    return *m_base_frequency_y;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-numoctaves
GC::Ref<SVGAnimatedInteger> SVGFETurbulenceElement::num_octaves()
{
    if (!m_num_octaves)
        m_num_octaves = SVGAnimatedInteger::create(realm(), *this, DOM::QualifiedName { AttributeNames::numOctaves, OptionalNone {}, OptionalNone {} }, 1);

    return *m_num_octaves;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-seed
GC::Ref<SVGAnimatedNumber> SVGFETurbulenceElement::seed()
{
    if (!m_seed)
        m_seed = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::seed, OptionalNone {}, OptionalNone {} }, 0);

    return *m_seed;
}

// https://drafts.csswg.org/filter-effects/#element-attrdef-feturbulence-stitchtiles
static SVGFETurbulenceElement::StitchType parse_stitch_tiles(String const& value)
{
    if (value == "stitch"sv)
        return SVGFETurbulenceElement::StitchType::Stitch;

    if (value == "noStitch"sv)
        return SVGFETurbulenceElement::StitchType::NoStitch;

    return SVGFETurbulenceElement::StitchType::NoStitch;
}

// https://drafts.csswg.org/filter-effects/#element-attrdef-feturbulence-stitchtiles
GC::Ref<SVGAnimatedEnumeration> SVGFETurbulenceElement::stitch_tiles()
{
    // FIXME: Support reflection, don't return a new object every time.
    auto stitch_tiles = parse_stitch_tiles(get_attribute_value(AttributeNames::stitchTiles));
    return SVGAnimatedEnumeration::create(realm(), to_underlying(stitch_tiles));
}

// https://drafts.csswg.org/filter-effects/#element-attrdef-feturbulence-type
static SVGFETurbulenceElement::TurbulenceType parse_turbulence_type(String const& value)
{
    if (value == "turbulence"sv)
        return SVGFETurbulenceElement::TurbulenceType::Turbulence;

    if (value == "fractalNoise"sv)
        return SVGFETurbulenceElement::TurbulenceType::FractalNoise;

    return SVGFETurbulenceElement::TurbulenceType::Turbulence;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-type
GC::Ref<SVGAnimatedEnumeration> SVGFETurbulenceElement::type()
{
    // FIXME: Support reflection, don't return a new object every time.
    auto turbulence_type = parse_turbulence_type(get_attribute_value(AttributeNames::type));
    return SVGAnimatedEnumeration::create(realm(), to_underlying(turbulence_type));
}

}
