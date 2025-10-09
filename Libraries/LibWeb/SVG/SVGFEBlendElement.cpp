/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEBlendElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEBlendElement);

SVGFEBlendElement::SVGFEBlendElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEBlendElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEBlendElement);
    Base::initialize(realm);
}

void SVGFEBlendElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_in2);
}

void SVGFEBlendElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, new_value, namespace_);

    if (name == SVG::AttributeNames::mode) {
        auto parse_mix_blend_mode = [](Optional<String> const& value) -> Optional<CSS::MixBlendMode> {
            if (!value.has_value())
                return {};
            auto keyword = CSS::keyword_from_string(*value);
            if (!keyword.has_value())
                return {};
            return CSS::keyword_to_mix_blend_mode(*keyword);
        };

        m_mode = parse_mix_blend_mode(new_value);
    }
}

GC::Ref<SVGAnimatedString> SVGFEBlendElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in);

    return *m_in1;
}

GC::Ref<SVGAnimatedString> SVGFEBlendElement::in2()
{
    if (!m_in2)
        m_in2 = SVGAnimatedString::create(realm(), *this, AttributeNames::in2);

    return *m_in2;
}

Gfx::CompositingAndBlendingOperator SVGFEBlendElement::mode() const
{
    return Painting::mix_blend_mode_to_compositing_and_blending_operator(m_mode.value_or(CSS::MixBlendMode::Normal));
}

GC::Ref<SVGAnimatedEnumeration> SVGFEBlendElement::mode_for_bindings() const
{
    return SVGAnimatedEnumeration::create(realm(), to_underlying(mode()));
}

}
