/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGGElement final : public SVGGraphicsElement {
    WEB_WRAPPABLE(SVGGElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGGElement);

public:
    virtual ~SVGGElement() override = default;

    virtual CSS::ElementBoxKind box_kind() const override;

private:
    SVGGElement(DOM::Document&, DOM::QualifiedName);
};

}
