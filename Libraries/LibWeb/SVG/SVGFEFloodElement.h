/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFEFloodElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEFloodElement> {
    WEB_WRAPPABLE(SVGFEFloodElement, SVGElement);

public:
    virtual ~SVGFEFloodElement() override = default;

    Gfx::Color flood_color();
    float flood_opacity() const;

private:
    explicit SVGFEFloodElement(DOM::RustMadePrefix);
    virtual void visit_edges(Cell::Visitor&) override;
};

}
