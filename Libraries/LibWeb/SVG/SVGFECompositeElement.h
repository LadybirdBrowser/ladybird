/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "SVGAnimatedNumber.h"

#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFECompositeElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFECompositeElement> {
    WEB_PLATFORM_OBJECT(SVGFECompositeElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFECompositeElement);

public:
    virtual ~SVGFECompositeElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedString> in2();

    GC::Ref<SVGAnimatedNumber> k1();
    GC::Ref<SVGAnimatedNumber> k2();
    GC::Ref<SVGAnimatedNumber> k3();
    GC::Ref<SVGAnimatedNumber> k4();

    enum class CompositingOperator : u8 {
        Unknown = 0,
        Over = 1,
        In = 2,
        Out = 3,
        Atop = 4,
        Xor = 5,
        Arithmetic = 6,
        Lighter = 7,
    };
    SVGFECompositeElement::CompositingOperator operator_() const;
    GC::Ref<SVGAnimatedEnumeration> operator_for_bindings() const;

private:
    SVGFECompositeElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedString> m_in2;

    GC::Ptr<SVGAnimatedNumber> m_k1;
    GC::Ptr<SVGAnimatedNumber> m_k2;
    GC::Ptr<SVGAnimatedNumber> m_k3;
    GC::Ptr<SVGAnimatedNumber> m_k4;

    Optional<CompositingOperator> m_operator;
};

}
