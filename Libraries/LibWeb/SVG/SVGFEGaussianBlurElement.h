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
    WEB_PLATFORM_OBJECT(SVGFEGaussianBlurElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEGaussianBlurElement);

public:
    virtual ~SVGFEGaussianBlurElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedNumber> std_deviation_x();
    GC::Ref<SVGAnimatedNumber> std_deviation_y();
    GC::Ref<SVGAnimatedEnumeration> edge_mode() const;

private:
    SVGFEGaussianBlurElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedNumber> m_std_deviation_x;
    GC::Ptr<SVGAnimatedNumber> m_std_deviation_y;
};

}
