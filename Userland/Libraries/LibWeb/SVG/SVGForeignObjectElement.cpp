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

JS_DEFINE_ALLOCATOR(SVGForeignObjectElement);

SVGForeignObjectElement::SVGForeignObjectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGForeignObjectElement::~SVGForeignObjectElement() = default;

void SVGForeignObjectElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGForeignObjectElement);

    // FIXME: These never actually get updated!
    m_x = SVGAnimatedLength::create(realm, SVGLength::create(realm, 0, 0), SVGLength::create(realm, 0, 0));
    m_y = SVGAnimatedLength::create(realm, SVGLength::create(realm, 0, 0), SVGLength::create(realm, 0, 0));
    m_width = SVGAnimatedLength::create(realm, SVGLength::create(realm, 0, 0), SVGLength::create(realm, 0, 0));
    m_height = SVGAnimatedLength::create(realm, SVGLength::create(realm, 0, 0), SVGLength::create(realm, 0, 0));
}

void SVGForeignObjectElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_width);
    visitor.visit(m_height);
}

JS::GCPtr<Layout::Node> SVGForeignObjectElement::create_layout_node(CSS::StyleProperties style)
{
    return heap().allocate_without_realm<Layout::SVGForeignObjectBox>(document(), *this, move(style));
}

void SVGForeignObjectElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    Base::apply_presentational_hints(style);
    auto parsing_context = CSS::Parser::ParsingContext { document() };
    if (auto width_value = parse_css_value(parsing_context, get_attribute_value(Web::HTML::AttributeNames::width), CSS::PropertyID::Width))
        style.set_property(CSS::PropertyID::Width, width_value.release_nonnull());

    if (auto height_value = parse_css_value(parsing_context, get_attribute_value(Web::HTML::AttributeNames::height), CSS::PropertyID::Height))
        style.set_property(CSS::PropertyID::Height, height_value.release_nonnull());
}

JS::NonnullGCPtr<SVG::SVGAnimatedLength> SVGForeignObjectElement::x()
{
    return *m_x;
}

JS::NonnullGCPtr<SVG::SVGAnimatedLength> SVGForeignObjectElement::y()
{
    return *m_y;
}

JS::NonnullGCPtr<SVG::SVGAnimatedLength> SVGForeignObjectElement::width()
{
    return *m_width;
}

JS::NonnullGCPtr<SVG::SVGAnimatedLength> SVGForeignObjectElement::height()
{
    return *m_height;
}

}
