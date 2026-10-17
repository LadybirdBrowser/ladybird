/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGAnimateMotionElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimateMotionElement);

SVGAnimateMotionElement::SVGAnimateMotionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGAnimationElement(document, move(qualified_name))
{
}

// https://svgwg.org/specs/animations/#AnimateMotionElement
Vector<Animations::BaseKeyframe> SVGAnimateMotionElement::keyframes(SVGElement const&, CSS::PropertyID) const
{
    // FIXME: The ‘animateMotion’ element causes a referenced element to move along a motion path.
    return {};
}

}
