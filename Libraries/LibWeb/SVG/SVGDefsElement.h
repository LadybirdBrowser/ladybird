/*
 * Copyright (c) 2022, Simon Danner <danner.simon@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGDefsElement final : public SVGGraphicsElement {
    WEB_WRAPPABLE(SVGDefsElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGDefsElement);

public:
    virtual ~SVGDefsElement();

    virtual CSS::ElementBoxKind box_kind() const override { return CSS::ElementBoxKind::NoBox; }

private:
    SVGDefsElement(DOM::Document&, DOM::QualifiedName);
};

}
