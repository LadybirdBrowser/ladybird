/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGForeignObjectElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/SVGForeignObjectBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGForeignObjectElement);

SVGForeignObjectElement::SVGForeignObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGForeignObjectElement::~SVGForeignObjectElement() = default;

void SVGForeignObjectElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGForeignObjectElement);
    Base::initialize(realm);

    // FIXME: These never actually get updated!
    m_x = fake_animated_length_fixme();
    m_y = fake_animated_length_fixme();
    m_width = fake_animated_length_fixme();
    m_height = fake_animated_length_fixme();
}

void SVGForeignObjectElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_width);
    visitor.visit(m_height);
}

GC::Ptr<Layout::Node> SVGForeignObjectElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGForeignObjectBox>(document(), *this, move(style));
}

bool SVGForeignObjectElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        SVG::AttributeNames::width,
        SVG::AttributeNames::height);
}

void SVGForeignObjectElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);
    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    if (auto width_value = parse_css_value(parsing_context, get_attribute_value(Web::HTML::AttributeNames::width), CSS::PropertyID::Width))
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, width_value.release_nonnull());

    if (auto height_value = parse_css_value(parsing_context, get_attribute_value(Web::HTML::AttributeNames::height), CSS::PropertyID::Height))
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, height_value.release_nonnull());
}

GC::Ref<SVG::SVGAnimatedLength> SVGForeignObjectElement::x()
{
    return *m_x;
}

GC::Ref<SVG::SVGAnimatedLength> SVGForeignObjectElement::y()
{
    return *m_y;
}

GC::Ref<SVG::SVGAnimatedLength> SVGForeignObjectElement::width()
{
    return *m_width;
}

GC::Ref<SVG::SVGAnimatedLength> SVGForeignObjectElement::height()
{
    return *m_height;
}

}
