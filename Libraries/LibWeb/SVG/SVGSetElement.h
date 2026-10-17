/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimationElement.h>

namespace Web::SVG {

class SVGSetElement final : public SVGAnimationElement {
    WEB_WRAPPABLE(SVGSetElement, SVGAnimationElement);
    GC_DECLARE_ALLOCATOR(SVGSetElement);

private:
    SVGSetElement(DOM::Document&, DOM::QualifiedName);
};

}
