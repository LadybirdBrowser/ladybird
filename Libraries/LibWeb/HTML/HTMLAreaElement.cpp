/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAreaElement);

HTMLAreaElement::HTMLAreaElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLAreaElement::~HTMLAreaElement() = default;

void HTMLAreaElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rel_list);
}

void HTMLAreaElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::href) {
        href_content_attribute_changed();
    } else if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.has_value() ? value->utf16_view() : u""sv);
    }

    if (name.is_one_of(HTML::AttributeNames::coords, HTML::AttributeNames::shape)) {
        if (is_focused())
            repaint_associated_images();
        document().set_image_map_areas_need_publication();
    }
}

void HTMLAreaElement::inserted()
{
    Base::inserted();
    document().set_image_map_areas_need_publication();
}

void HTMLAreaElement::removed_from(IsSubtreeRoot is_subtree_root, DOM::Node* old_ancestor, DOM::Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    document().set_image_map_areas_need_publication();
}

bool HTMLAreaElement::has_activation_behavior() const
{
    return creates_a_hyperlink();
}

void HTMLAreaElement::activation_behavior(Web::DOM::Event const& event)
{
    activate_the_hyperlink(event);
}

// https://html.spec.whatwg.org/multipage/image-maps.html#attr-area-shape
HTMLAreaElement::ShapeState HTMLAreaElement::shape_state() const
{
    // The shape attribute is an enumerated attribute with the following keywords and states:
    // circle, circ: Circle state
    // default: Default state
    // poly, polygon: Polygon state
    // rect, rectangle: Rectangle state
    auto shape = attribute(HTML::AttributeNames::shape);
    if (shape.has_value()) {
        if (shape->utf16_view().is_one_of_ignoring_ascii_case("circle"sv, "circ"sv))
            return ShapeState::Circle;
        if (shape->utf16_view().equals_ignoring_ascii_case("default"sv))
            return ShapeState::Default;
        if (shape->utf16_view().is_one_of_ignoring_ascii_case("poly"sv, "polygon"sv))
            return ShapeState::Polygon;
    }

    // The attribute's missing value default and invalid value default are both the rectangle state.
    return ShapeState::Rectangle;
}

// https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
Vector<double> HTMLAreaElement::shape_coords() const
{
    // 2. Use the rules for parsing a list of floating-point numbers to parse the element's coords attribute, if it
    //    is present, and let the coords list be the result. If the attribute is absent, let the coords list be the
    //    empty list.
    auto coords_attribute = attribute(HTML::AttributeNames::coords);
    if (!coords_attribute.has_value())
        return {};
    return parse_list_of_floating_point_numbers(coords_attribute->utf16_view());
}

// https://html.spec.whatwg.org/multipage/image-maps.html#dom-area-rellist
GC::Ref<DOM::DOMTokenList> HTMLAreaElement::rel_list()
{
    // The IDL attribute relList must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLAreaElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

// https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
bool HTMLAreaElement::is_focusable() const
{
    // NB: Platform conventions decide whether an area element without a tabindex attribute is a focusable area. Like
    //     other engines, only area elements that create a hyperlink are considered focusable areas.
    if (!creates_a_hyperlink() && !HTML::parse_integer(attribute(HTML::AttributeNames::tabindex).value_or({})).has_value())
        return false;

    // The shapes of area elements in an image map associated with an img element that is being rendered and is not
    // inert.
    // NB: Area elements are never rendered themselves, so the rendering and inertness requirements apply to an
    //     associated image element instead.
    auto const* map_element = first_ancestor_of_type<HTMLMapElement>();
    return map_element && map_element->first_image_with_focusable_shapes();
}

void HTMLAreaElement::did_receive_focus()
{
    repaint_associated_images();
}

void HTMLAreaElement::did_lose_focus()
{
    repaint_associated_images();
}

void HTMLAreaElement::repaint_associated_images()
{
    auto const* map_element = first_ancestor_of_type<HTMLMapElement>();
    if (!map_element)
        return;

    map_element->for_each_associated_image([](HTMLImageElement& image_element) {
        image_element.set_needs_repaint();
        return TraversalDecision::Continue;
    });
}

Optional<ARIA::Role> HTMLAreaElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-area-no-href
    if (!href().is_empty())
        return ARIA::Role::link;
    // https://www.w3.org/TR/html-aria/#el-area
    return ARIA::Role::generic;
}

}
