/*
 * Copyright (c) 2026, Samuele Cerea <samu@cerea.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEDisplacementMapElementPrototype.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEDisplacementMapElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEDisplacementMapElement);

SVGFEDisplacementMapElement::SVGFEDisplacementMapElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEDisplacementMapElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEDisplacementMapElement);
    Base::initialize(realm);
}

void SVGFEDisplacementMapElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_in2);
    visitor.visit(m_scale);
}

static SVGFEDisplacementMapElement::ChannelSelector string_to_channel_selector(StringView string)
{
    if (string.length() != 1)
        return SVGFEDisplacementMapElement::ChannelSelector::Unknown;

    switch (string[0]) {
    case 'r':
    case 'R':
        return SVGFEDisplacementMapElement::ChannelSelector::R;
    case 'g':
    case 'G':
        return SVGFEDisplacementMapElement::ChannelSelector::G;
    case 'b':
    case 'B':
        return SVGFEDisplacementMapElement::ChannelSelector::B;
    case 'a':
    case 'A':
        return SVGFEDisplacementMapElement::ChannelSelector::A;
    default:
        return SVGFEDisplacementMapElement::ChannelSelector::Unknown;
    }
}

void SVGFEDisplacementMapElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, new_value, namespace_);

    if (name == SVG::AttributeNames::xChannelSelector) {
        m_x_channel_selector = new_value.map([](auto const& value) {
            return string_to_channel_selector(value);
        });
    } else if (name == SVG::AttributeNames::yChannelSelector) {
        m_y_channel_selector = new_value.map([](auto const& value) {
            return string_to_channel_selector(value);
        });
    }
}

GC::Ref<SVGAnimatedString> SVGFEDisplacementMapElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });

    return *m_in1;
}

GC::Ref<SVGAnimatedString> SVGFEDisplacementMapElement::in2()
{
    if (!m_in2)
        m_in2 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in2, OptionalNone {}, OptionalNone {} });

    return *m_in2;
}

GC::Ref<SVGAnimatedNumber> SVGFEDisplacementMapElement::scale()
{
    if (!m_scale)
        m_scale = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::surfaceScale, OptionalNone {}, OptionalNone {} }, 0.f);

    return *m_scale;
}

SVGFEDisplacementMapElement::ChannelSelector SVGFEDisplacementMapElement::x_channel_selector() const
{
    return m_x_channel_selector.value_or(ChannelSelector::A);
}

SVGFEDisplacementMapElement::ChannelSelector SVGFEDisplacementMapElement::y_channel_selector() const
{
    return m_y_channel_selector.value_or(ChannelSelector::A);
}

GC::Ref<SVGAnimatedEnumeration> SVGFEDisplacementMapElement::x_channel_selector_bindings() const
{
    return SVGAnimatedEnumeration::create(realm(), to_underlying(x_channel_selector()));
}

GC::Ref<SVGAnimatedEnumeration> SVGFEDisplacementMapElement::y_channel_selector_bindings() const
{
    return SVGAnimatedEnumeration::create(realm(), to_underlying(y_channel_selector()));
}

}
