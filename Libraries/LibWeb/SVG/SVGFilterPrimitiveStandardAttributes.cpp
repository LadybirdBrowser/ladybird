/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>
#include <LibWeb/SVG/SVGFEColorMatrixElement.h>
#include <LibWeb/SVG/SVGFEComponentTransferElement.h>
#include <LibWeb/SVG/SVGFECompositeElement.h>
#include <LibWeb/SVG/SVGFEDropShadowElement.h>
#include <LibWeb/SVG/SVGFEFloodElement.h>
#include <LibWeb/SVG/SVGFEGaussianBlurElement.h>
#include <LibWeb/SVG/SVGFEImageElement.h>
#include <LibWeb/SVG/SVGFEMergeElement.h>
#include <LibWeb/SVG/SVGFEMorphologyElement.h>
#include <LibWeb/SVG/SVGFEOffsetElement.h>
#include <LibWeb/SVG/SVGFETurbulenceElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

template<typename IncludingClass>
GC::Ref<SVGAnimatedLength> SVGFilterPrimitiveStandardAttributes<IncludingClass>::x()
{
    return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::X);
}

template<typename IncludingClass>
GC::Ref<SVGAnimatedLength> SVGFilterPrimitiveStandardAttributes<IncludingClass>::y()
{
    return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::Y);
}

template<typename IncludingClass>
GC::Ref<SVGAnimatedLength> SVGFilterPrimitiveStandardAttributes<IncludingClass>::width()
{
    return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::Width);
}

template<typename IncludingClass>
GC::Ref<SVGAnimatedLength> SVGFilterPrimitiveStandardAttributes<IncludingClass>::height()
{
    return this_svg_element()->svg_animated_length_for_property(CSS::PropertyID::Height);
}

template<typename IncludingClass>
GC::Ref<SVGAnimatedString> SVGFilterPrimitiveStandardAttributes<IncludingClass>::result()
{
    if (!m_result_animated_string)
        m_result_animated_string = SVGAnimatedString::create(this_svg_element()->realm(), *this_svg_element(), DOM::QualifiedName { AttributeNames::result, OptionalNone {}, OptionalNone {} });

    return *m_result_animated_string;
}

template class SVGFilterPrimitiveStandardAttributes<SVGFEBlendElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEColorMatrixElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEComponentTransferElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFECompositeElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEDropShadowElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEFloodElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEGaussianBlurElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEImageElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEMergeElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEMorphologyElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFEOffsetElement>;
template class SVGFilterPrimitiveStandardAttributes<SVGFETurbulenceElement>;

}
