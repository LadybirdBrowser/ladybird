/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGSymbolElementPrototype.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>
#include <LibWeb/SVG/SVGSymbolElement.h>
#include <LibWeb/SVG/SVGUseElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGSymbolElement);

SVGSymbolElement::SVGSymbolElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, qualified_name)
{
}

void SVGSymbolElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGSymbolElement);
    m_view_box_for_bindings = heap().allocate<SVGAnimatedRect>(realm, realm);
}

void SVGSymbolElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_view_box_for_bindings);
}

// https://svgwg.org/svg2-draft/struct.html#SymbolNotes
void SVGSymbolElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    Base::apply_presentational_hints(style);

    if (is_direct_child_of_use_shadow_tree()) {
        // The generated instance of a ‘symbol’ that is the direct referenced element of a ‘use’ element must always have a computed value of inline for the display property.
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Inline)));
    }
}

void SVGSymbolElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    Base::attribute_changed(name, old_value, value);
    if (name.equals_ignoring_ascii_case(SVG::AttributeNames::viewBox)) {
        m_view_box = try_parse_view_box(value.value_or(String {}));
        m_view_box_for_bindings->set_nulled(!m_view_box.has_value());
        if (m_view_box.has_value()) {
            m_view_box_for_bindings->set_base_val(Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y, m_view_box->width, m_view_box->height });
            m_view_box_for_bindings->set_anim_val(Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y, m_view_box->width, m_view_box->height });
        }
    }
}

bool SVGSymbolElement::is_direct_child_of_use_shadow_tree() const
{
    auto maybe_shadow_root = parent();
    if (!is<DOM::ShadowRoot>(maybe_shadow_root)) {
        return false;
    }

    auto host = static_cast<const DOM::ShadowRoot&>(*maybe_shadow_root).host();
    return is<SVGUseElement>(host);
}

JS::GCPtr<Layout::Node> SVGSymbolElement::create_layout_node(NonnullRefPtr<CSS::StyleProperties> style)
{
    return heap().allocate_without_realm<Layout::SVGGraphicsBox>(document(), *this, move(style));
}

}
