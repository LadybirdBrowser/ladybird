/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGStopElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGStopElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGStopElement);

SVGStopElement::SVGStopElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGStopElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::offset) {
        m_offset = AttributeParser::parse_number_percentage(value.value_or(String {}));
    }
}

bool SVGStopElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        "stop-color"sv,
        "stop-opacity"sv);
}

void SVGStopElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    CSS::Parser::ParsingParams parsing_context { document() };
    for_each_attribute([&](auto& name, auto& value) {
        CSS::Parser::ParsingParams parsing_context { document() };
        if (name.equals_ignoring_ascii_case("stop-color"sv)) {
            if (auto stop_color = parse_css_value(parsing_context, value, CSS::PropertyID::StopColor)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::StopColor, stop_color.release_nonnull());
            }
        } else if (name.equals_ignoring_ascii_case("stop-opacity"sv)) {
            if (auto stop_opacity = parse_css_value(parsing_context, value, CSS::PropertyID::StopOpacity)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::StopOpacity, stop_opacity.release_nonnull());
            }
        }
    });
}

Gfx::Color SVGStopElement::stop_color() const
{
    if (auto computed_properties = this->computed_properties())
        return computed_properties->stop_color();
    return Color::Black;
}

float SVGStopElement::stop_opacity() const
{
    if (auto computed_properties = this->computed_properties())
        return computed_properties->stop_opacity();
    return 1;
}

GC::Ref<SVGAnimatedNumber> SVGStopElement::offset() const
{
    // FIXME: Implement this properly.
    return SVGAnimatedNumber::create(realm(), 0, 0);
}

void SVGStopElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGStopElement);
}

}
