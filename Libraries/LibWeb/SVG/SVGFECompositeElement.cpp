/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFECompositeElementPrototype.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFECompositeElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFECompositeElement);

SVGFECompositeElement::SVGFECompositeElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFECompositeElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFECompositeElement);
    Base::initialize(realm);
}

void SVGFECompositeElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_in2);
    visitor.visit(m_k1);
    visitor.visit(m_k2);
    visitor.visit(m_k3);
    visitor.visit(m_k4);
}

static SVGFECompositeElement::CompositingOperator string_to_compositing_operator(StringView string)
{
    if (string == "over"sv)
        return SVGFECompositeElement::CompositingOperator::Over;
    if (string == "in"sv)
        return SVGFECompositeElement::CompositingOperator::In;
    if (string == "out"sv)
        return SVGFECompositeElement::CompositingOperator::Out;
    if (string == "atop"sv)
        return SVGFECompositeElement::CompositingOperator::Atop;
    if (string == "xor"sv)
        return SVGFECompositeElement::CompositingOperator::Xor;
    if (string == "arithmetic"sv)
        return SVGFECompositeElement::CompositingOperator::Arithmetic;
    return SVGFECompositeElement::CompositingOperator::Unknown;
}

void SVGFECompositeElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, new_value, namespace_);

    if (name == SVG::AttributeNames::operator_) {
        auto parse_compositing_operator = [](Optional<String> const& value) -> Optional<CompositingOperator> {
            if (!value.has_value())
                return {};
            return string_to_compositing_operator(*value);
        };

        m_operator = parse_compositing_operator(new_value);
    }
}

GC::Ref<SVGAnimatedString> SVGFECompositeElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in);

    return *m_in1;
}

GC::Ref<SVGAnimatedString> SVGFECompositeElement::in2()
{
    if (!m_in2)
        m_in2 = SVGAnimatedString::create(realm(), *this, AttributeNames::in2);

    return *m_in2;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fecomposite-k1
GC::Ref<SVGAnimatedNumber> SVGFECompositeElement::k1()
{
    if (!m_k1)
        m_k1 = SVGAnimatedNumber::create(realm(), *this, AttributeNames::k1, 0.f);

    return *m_k1;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fecomposite-k2
GC::Ref<SVGAnimatedNumber> SVGFECompositeElement::k2()
{
    if (!m_k2)
        m_k2 = SVGAnimatedNumber::create(realm(), *this, AttributeNames::k2, 0.f);

    return *m_k2;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fecomposite-k3
GC::Ref<SVGAnimatedNumber> SVGFECompositeElement::k3()
{
    if (!m_k3)
        m_k3 = SVGAnimatedNumber::create(realm(), *this, AttributeNames::k3, 0.f);

    return *m_k3;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fecomposite-k4
GC::Ref<SVGAnimatedNumber> SVGFECompositeElement::k4()
{
    if (!m_k4)
        m_k4 = SVGAnimatedNumber::create(realm(), *this, AttributeNames::k4, 0.f);

    return *m_k4;
}

SVGFECompositeElement::CompositingOperator SVGFECompositeElement::operator_() const
{
    return m_operator.value_or(CompositingOperator::Over);
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fecomposite-operator
GC::Ref<SVGAnimatedEnumeration> SVGFECompositeElement::operator_for_bindings() const
{
    return SVGAnimatedEnumeration::create(realm(), to_underlying(m_operator.value_or(CompositingOperator::Over)));
}

}
