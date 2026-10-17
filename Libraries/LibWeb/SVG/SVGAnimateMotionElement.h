/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimationElement.h>

namespace Web::SVG {

class SVGAnimateMotionElement final : public SVGAnimationElement {
    WEB_WRAPPABLE(SVGAnimateMotionElement, SVGAnimationElement);
    GC_DECLARE_ALLOCATOR(SVGAnimateMotionElement);

private:
    SVGAnimateMotionElement(DOM::Document&, DOM::QualifiedName);

    virtual Vector<Animations::BaseKeyframe> keyframes(SVGElement const& target, CSS::PropertyID) const override;
};

}
