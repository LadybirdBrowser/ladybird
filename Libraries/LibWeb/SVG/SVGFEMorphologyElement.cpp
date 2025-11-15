/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEMorphologyElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFEMorphologyElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEMorphologyElement);

SVGFEMorphologyElement::SVGFEMorphologyElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEMorphologyElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEMorphologyElement);
    Base::initialize(realm);
}

void SVGFEMorphologyElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_radius_x);
    visitor.visit(m_radius_y);
}

void SVGFEMorphologyElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, new_value, namespace_);

    if (name == SVG::AttributeNames::operator_) {
        if (!new_value.has_value()) {
            m_morphology_operator = Gfx::MorphologyOperator::Dilate;
            return;
        }
        if (new_value->equals_ignoring_ascii_case("erode"sv)) {
            m_morphology_operator = Gfx::MorphologyOperator::Erode;
        } else if (new_value->equals_ignoring_ascii_case("dilate"sv)) {
            m_morphology_operator = Gfx::MorphologyOperator::Dilate;
        } else {
            m_morphology_operator = Gfx::MorphologyOperator::Dilate;
        }
    }
}

GC::Ref<SVGAnimatedString> SVGFEMorphologyElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });

    return *m_in1;
}

GC::Ref<SVGAnimatedEnumeration> SVGFEMorphologyElement::operator_for_bindings()
{
    return SVGAnimatedEnumeration::create(realm(), to_underlying(m_morphology_operator));
}

GC::Ref<SVGAnimatedNumber> SVGFEMorphologyElement::radius_x()
{
    if (!m_radius_x)
        m_radius_x = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { SVG::AttributeNames::radius, OptionalNone {}, OptionalNone {} }, 0.0,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);

    return *m_radius_x;
}

GC::Ref<SVGAnimatedNumber> SVGFEMorphologyElement::radius_y()
{
    if (!m_radius_y)
        m_radius_y = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { SVG::AttributeNames::radius, OptionalNone {}, OptionalNone {} }, 0.0,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);

    return *m_radius_y;
}

}
