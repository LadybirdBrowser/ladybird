/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEConvolveMatrixElement.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEConvolveMatrixElement);

SVGFEConvolveMatrixElement::SVGFEConvolveMatrixElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEConvolveMatrixElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_order_x);
    visitor.visit(m_order_y);
    visitor.visit(m_kernel_matrix);
    visitor.visit(m_divisor);
    visitor.visit(m_bias);
    visitor.visit(m_target_x);
    visitor.visit(m_target_y);
    visitor.visit(m_kernel_unit_length_x);
    visitor.visit(m_kernel_unit_length_y);
    visitor.visit(m_preserve_alpha);
}

void SVGFEConvolveMatrixElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::kernelMatrix && m_kernel_matrix)
        m_kernel_matrix->synchronize();
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-in1
GC::Ref<SVGAnimatedString> SVGFEConvolveMatrixElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });
    return *m_in1;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-orderx
GC::Ref<SVGAnimatedInteger> SVGFEConvolveMatrixElement::order_x()
{
    if (!m_order_x)
        m_order_x = SVGAnimatedInteger::create(*this, DOM::QualifiedName { AttributeNames::order, OptionalNone {}, OptionalNone {} }, 3,
            SVGAnimatedInteger::SupportsSecondValue::Yes, SVGAnimatedInteger::ValueRepresented::First);
    return *m_order_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-ordery
GC::Ref<SVGAnimatedInteger> SVGFEConvolveMatrixElement::order_y()
{
    if (!m_order_y)
        m_order_y = SVGAnimatedInteger::create(*this, DOM::QualifiedName { AttributeNames::order, OptionalNone {}, OptionalNone {} }, 3,
            SVGAnimatedInteger::SupportsSecondValue::Yes, SVGAnimatedInteger::ValueRepresented::Second);
    return *m_order_y;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-kernelmatrix
GC::Ref<SVGAnimatedNumberList> SVGFEConvolveMatrixElement::kernel_matrix()
{
    if (!m_kernel_matrix)
        m_kernel_matrix = SVGAnimatedNumberList::create(*this, AttributeNames::kernelMatrix);
    return *m_kernel_matrix;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-divisor
GC::Ref<SVGAnimatedNumber> SVGFEConvolveMatrixElement::divisor()
{
    if (!m_divisor)
        m_divisor = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::divisor, OptionalNone {}, OptionalNone {} }, 1.f);
    return *m_divisor;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-bias
GC::Ref<SVGAnimatedNumber> SVGFEConvolveMatrixElement::bias()
{
    if (!m_bias)
        m_bias = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::bias, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_bias;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-targetx
GC::Ref<SVGAnimatedInteger> SVGFEConvolveMatrixElement::target_x()
{
    if (!m_target_x)
        m_target_x = SVGAnimatedInteger::create(*this, DOM::QualifiedName { AttributeNames::targetX, OptionalNone {}, OptionalNone {} }, 0);
    return *m_target_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-targety
GC::Ref<SVGAnimatedInteger> SVGFEConvolveMatrixElement::target_y()
{
    if (!m_target_y)
        m_target_y = SVGAnimatedInteger::create(*this, DOM::QualifiedName { AttributeNames::targetY, OptionalNone {}, OptionalNone {} }, 0);
    return *m_target_y;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-edgemode
GC::Ref<SVGAnimatedEnumeration> SVGFEConvolveMatrixElement::edge_mode()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "duplicate"sv, SVG_EDGEMODE_DUPLICATE },
        SVGAnimatedEnumeration::Keyword { "wrap"sv, SVG_EDGEMODE_WRAP },
        SVGAnimatedEnumeration::Keyword { "none"sv, SVG_EDGEMODE_NONE },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::edgeMode, keywords, SVG_EDGEMODE_DUPLICATE);
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-kernelunitlengthx
GC::Ref<SVGAnimatedNumber> SVGFEConvolveMatrixElement::kernel_unit_length_x()
{
    if (!m_kernel_unit_length_x)
        m_kernel_unit_length_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::kernelUnitLength, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    return *m_kernel_unit_length_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-kernelunitlengthy
GC::Ref<SVGAnimatedNumber> SVGFEConvolveMatrixElement::kernel_unit_length_y()
{
    if (!m_kernel_unit_length_y)
        m_kernel_unit_length_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::kernelUnitLength, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    return *m_kernel_unit_length_y;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfeconvolvematrixelement-preservealpha
GC::Ref<SVGAnimatedBoolean> SVGFEConvolveMatrixElement::preserve_alpha()
{
    if (!m_preserve_alpha)
        m_preserve_alpha = SVGAnimatedBoolean::create(*this, AttributeNames::preserveAlpha, false);
    return *m_preserve_alpha;
}

}
