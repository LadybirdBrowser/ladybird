/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFEGaussianBlurElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEGaussianBlurElement> {
    WEB_WRAPPABLE(SVGFEGaussianBlurElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEGaussianBlurElement);

public:
    static constexpr u16 SVG_EDGEMODE_UNKNOWN = 0;
    static constexpr u16 SVG_EDGEMODE_DUPLICATE = 1;
    static constexpr u16 SVG_EDGEMODE_WRAP = 2;
    static constexpr u16 SVG_EDGEMODE_NONE = 3;

    virtual ~SVGFEGaussianBlurElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedNumber> std_deviation_x();
    GC::Ref<SVGAnimatedNumber> std_deviation_y();
    GC::Ref<SVGAnimatedEnumeration> edge_mode();

private:
    SVGFEGaussianBlurElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedNumber> m_std_deviation_x;
    GC::Ptr<SVGAnimatedNumber> m_std_deviation_y;
};

}
