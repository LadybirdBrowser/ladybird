/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEBlendElement.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEBlendElement);

SVGFEBlendElement::SVGFEBlendElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
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

void SVGFEBlendElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, new_value, namespace_);

    if (name == SVG::AttributeNames::mode) {
        auto parse_mix_blend_mode = [](Optional<Utf16String> const& value) -> Optional<CSS::MixBlendMode> {
            if (!value.has_value())
                return {};
            if (*value == "normal"sv)
                return CSS::MixBlendMode::Normal;
            if (*value == "multiply"sv)
                return CSS::MixBlendMode::Multiply;
            if (*value == "screen"sv)
                return CSS::MixBlendMode::Screen;
            if (*value == "overlay"sv)
                return CSS::MixBlendMode::Overlay;
            if (*value == "darken"sv)
                return CSS::MixBlendMode::Darken;
            if (*value == "lighten"sv)
                return CSS::MixBlendMode::Lighten;
            if (*value == "color-dodge"sv)
                return CSS::MixBlendMode::ColorDodge;
            if (*value == "color-burn"sv)
                return CSS::MixBlendMode::ColorBurn;
            if (*value == "hard-light"sv)
                return CSS::MixBlendMode::HardLight;
            if (*value == "soft-light"sv)
                return CSS::MixBlendMode::SoftLight;
            if (*value == "difference"sv)
                return CSS::MixBlendMode::Difference;
            if (*value == "exclusion"sv)
                return CSS::MixBlendMode::Exclusion;
            if (*value == "hue"sv)
                return CSS::MixBlendMode::Hue;
            if (*value == "saturation"sv)
                return CSS::MixBlendMode::Saturation;
            if (*value == "color"sv)
                return CSS::MixBlendMode::Color;
            if (*value == "luminosity"sv)
                return CSS::MixBlendMode::Luminosity;
            if (*value == "plus-darker"sv)
                return CSS::MixBlendMode::PlusDarker;
            if (*value == "plus-lighter"sv)
                return CSS::MixBlendMode::PlusLighter;
            return {};
        };

        m_mode = parse_mix_blend_mode(new_value);
    }
}

GC::Ref<SVGAnimatedString> SVGFEBlendElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });

    return *m_in1;
}

GC::Ref<SVGAnimatedString> SVGFEBlendElement::in2()
{
    if (!m_in2)
        m_in2 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in2, OptionalNone {}, OptionalNone {} });

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
