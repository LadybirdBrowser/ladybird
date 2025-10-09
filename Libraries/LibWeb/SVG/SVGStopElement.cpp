/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGStopElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGStopElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGStopElement);

SVGStopElement::SVGStopElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

bool SVGStopElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name, SVG::AttributeNames::stopColor, SVG::AttributeNames::stopOpacity);
}

void SVGStopElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    CSS::Parser::ParsingParams parsing_context { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    for_each_attribute([&](auto& name, auto& value) {
        if (name == SVG::AttributeNames::stopColor) {
            if (auto stop_color = parse_css_value(parsing_context, value, CSS::PropertyID::StopColor)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::StopColor, stop_color.release_nonnull());
            }
        } else if (name == SVG::AttributeNames::stopOpacity) {
            if (auto stop_opacity = parse_css_value(parsing_context, value, CSS::PropertyID::StopOpacity)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::StopOpacity, stop_opacity.release_nonnull());
            }
        }
    });
}

Gfx::Color SVGStopElement::stop_color()
{
    if (auto computed_properties = this->computed_properties())
        return computed_properties->color_or_fallback(CSS::PropertyID::StopColor, CSS::ColorResolutionContext::for_element({ *this }), CSS::InitialValues::stop_color());
    return CSS::InitialValues::stop_color();
}

float SVGStopElement::stop_opacity() const
{
    if (auto computed_properties = this->computed_properties())
        return computed_properties->stop_opacity();
    return 1;
}

// https://svgwg.org/svg2-draft/pservers.html#StopElementOffsetAttribute
GC::Ref<SVGAnimatedNumber> SVGStopElement::offset()
{
    if (!m_stop_offset)
        m_stop_offset = SVGAnimatedNumber::create(realm(), *this, AttributeNames::offset, 0.f);
    return *m_stop_offset;
}

void SVGStopElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGStopElement);
    Base::initialize(realm);
}

void SVGStopElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stop_offset);
}

}
