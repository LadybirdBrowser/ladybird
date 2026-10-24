/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFETurbulenceElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFETurbulenceElement);

SVGFETurbulenceElement::SVGFETurbulenceElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFETurbulenceElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_base_frequency_x);
    visitor.visit(m_base_frequency_y);
    visitor.visit(m_num_octaves);
    visitor.visit(m_seed);
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-basefrequencyx
GC::Ref<SVGAnimatedNumber> SVGFETurbulenceElement::base_frequency_x()
{
    if (!m_base_frequency_x) {
        m_base_frequency_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::baseFrequency, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    }
    return *m_base_frequency_x;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-basefrequencyy
GC::Ref<SVGAnimatedNumber> SVGFETurbulenceElement::base_frequency_y()
{
    if (!m_base_frequency_y) {
        m_base_frequency_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::baseFrequency, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    }
    return *m_base_frequency_y;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-numoctaves
GC::Ref<SVGAnimatedInteger> SVGFETurbulenceElement::num_octaves()
{
    if (!m_num_octaves)
        m_num_octaves = SVGAnimatedInteger::create(*this, DOM::QualifiedName { AttributeNames::numOctaves, OptionalNone {}, OptionalNone {} }, 1);

    return *m_num_octaves;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-seed
GC::Ref<SVGAnimatedNumber> SVGFETurbulenceElement::seed()
{
    if (!m_seed)
        m_seed = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::seed, OptionalNone {}, OptionalNone {} }, 0);

    return *m_seed;
}

// https://drafts.csswg.org/filter-effects/#element-attrdef-feturbulence-stitchtiles
GC::Ref<SVGAnimatedEnumeration> SVGFETurbulenceElement::stitch_tiles()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "stitch"sv, to_underlying(StitchType::Stitch) },
        SVGAnimatedEnumeration::Keyword { "noStitch"sv, to_underlying(StitchType::NoStitch) },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::stitchTiles, keywords, to_underlying(StitchType::NoStitch));
}

// https://drafts.csswg.org/filter-effects/#dom-svgfeturbulenceelement-type
GC::Ref<SVGAnimatedEnumeration> SVGFETurbulenceElement::type()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "fractalNoise"sv, to_underlying(TurbulenceType::FractalNoise) },
        SVGAnimatedEnumeration::Keyword { "turbulence"sv, to_underlying(TurbulenceType::Turbulence) },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::type, keywords, to_underlying(TurbulenceType::Turbulence));
}

}
