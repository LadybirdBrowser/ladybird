/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedBoolean.h>
#include <LibWeb/SVG/SVGAnimatedInteger.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGAnimatedNumberList.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://drafts.fxtf.org/filter-effects/#InterfaceSVGFEConvolveMatrixElement
class SVGFEConvolveMatrixElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEConvolveMatrixElement> {
    WEB_WRAPPABLE(SVGFEConvolveMatrixElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEConvolveMatrixElement);

public:
    static constexpr u16 SVG_EDGEMODE_UNKNOWN = 0;
    static constexpr u16 SVG_EDGEMODE_DUPLICATE = 1;
    static constexpr u16 SVG_EDGEMODE_WRAP = 2;
    static constexpr u16 SVG_EDGEMODE_NONE = 3;

    virtual ~SVGFEConvolveMatrixElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedInteger> order_x();
    GC::Ref<SVGAnimatedInteger> order_y();
    GC::Ref<SVGAnimatedNumberList> kernel_matrix();
    GC::Ref<SVGAnimatedNumber> divisor();
    GC::Ref<SVGAnimatedNumber> bias();
    GC::Ref<SVGAnimatedInteger> target_x();
    GC::Ref<SVGAnimatedInteger> target_y();
    GC::Ref<SVGAnimatedEnumeration> edge_mode();
    GC::Ref<SVGAnimatedNumber> kernel_unit_length_x();
    GC::Ref<SVGAnimatedNumber> kernel_unit_length_y();
    GC::Ref<SVGAnimatedBoolean> preserve_alpha();

private:
    SVGFEConvolveMatrixElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedInteger> m_order_x;
    GC::Ptr<SVGAnimatedInteger> m_order_y;
    GC::Ptr<SVGAnimatedNumberList> m_kernel_matrix;
    GC::Ptr<SVGAnimatedNumber> m_divisor;
    GC::Ptr<SVGAnimatedNumber> m_bias;
    GC::Ptr<SVGAnimatedInteger> m_target_x;
    GC::Ptr<SVGAnimatedInteger> m_target_y;
    GC::Ptr<SVGAnimatedNumber> m_kernel_unit_length_x;
    GC::Ptr<SVGAnimatedNumber> m_kernel_unit_length_y;
    GC::Ptr<SVGAnimatedBoolean> m_preserve_alpha;
};

}
