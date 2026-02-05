/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/MorphologyOperator.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#svgfemorphologyelement
class SVGFEMorphologyElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEMorphologyElement> {
    WEB_PLATFORM_OBJECT(SVGFEMorphologyElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEMorphologyElement);

public:
    virtual ~SVGFEMorphologyElement() override = default;

    GC::Ref<SVGAnimatedString> in1();

    GC::Ref<SVGAnimatedEnumeration> operator_for_bindings();
    Gfx::MorphologyOperator morphology_operator() { return m_morphology_operator; }

    GC::Ref<SVGAnimatedNumber> radius_x();
    GC::Ref<SVGAnimatedNumber> radius_y();

private:
    SVGFEMorphologyElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    Gfx::MorphologyOperator m_morphology_operator { Gfx::MorphologyOperator::Erode };
    GC::Ptr<SVGAnimatedNumber> m_radius_x;
    GC::Ptr<SVGAnimatedNumber> m_radius_y;
};

}
