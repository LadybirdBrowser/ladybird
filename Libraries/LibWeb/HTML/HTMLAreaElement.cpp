/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
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
        set_the_url();
    } else if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.has_value() ? value->utf16_view() : u""sv);
    }

    if (is_focused() && name.is_one_of(HTML::AttributeNames::coords, HTML::AttributeNames::shape))
        repaint_associated_images();
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
Optional<Gfx::Path> HTMLAreaElement::shape_path(CSSPixelSize image_size) const
{
    // Each area element in areas must be processed as follows to obtain a shape to layer onto the image:

    // 1. Find the state that the element's shape attribute represents.
    auto shape = shape_state();

    // 2. Use the rules for parsing a list of floating-point numbers to parse the element's coords attribute, if it
    //    is present, and let the coords list be the result. If the attribute is absent, let the coords list be the
    //    empty list.
    Vector<double> coords;
    if (auto coords_attribute = attribute(HTML::AttributeNames::coords); coords_attribute.has_value())
        coords = parse_list_of_floating_point_numbers(coords_attribute->utf16_view());

    auto vertex = [&](size_t index) {
        return Gfx::FloatPoint { static_cast<float>(coords[2 * index]), static_cast<float>(coords[2 * index + 1]) };
    };

    auto rectangle_path = [](Gfx::FloatPoint top_left, Gfx::FloatPoint bottom_right) {
        Gfx::Path path;
        path.move_to(top_left);
        path.line_to({ bottom_right.x(), top_left.y() });
        path.line_to(bottom_right);
        path.line_to({ top_left.x(), bottom_right.y() });
        path.close();
        return path;
    };

    // 3. If the number of items in the coords list is less than the minimum number given for the area element's
    //    current state, as per the following table, then the shape is empty; return.
    // 4. Check for excess items in the coords list as per the entry in the following list corresponding to the
    //    shape attribute's state:
    // NB: Excess items require no handling, because each shape reads only the items it requires.
    // 8. Now, the shape represented by the element is the one described for the entry in the list below
    //    corresponding to the state of the shape attribute:
    switch (shape) {
    case ShapeState::Circle: {
        if (coords.size() < 3)
            return {};

        // 7. If the shape attribute represents the circle state, and the third number in the list is less than or
        //    equal to zero, then the shape is empty; return.
        if (coords[2] <= 0)
            return {};

        // Let x be the first number in coords, y be the second number, and r be the third number.
        // The shape is a circle whose center is x CSS pixels from the left edge of the image and y CSS pixels from
        // the top edge of the image, and whose radius is r CSS pixels.
        auto x = static_cast<float>(coords[0]);
        auto y = static_cast<float>(coords[1]);
        auto radius = static_cast<float>(coords[2]);
        Gfx::Path path;
        path.move_to({ x + radius, y });
        path.arc_to({ x, y + radius }, radius, false, true);
        path.arc_to({ x - radius, y }, radius, false, true);
        path.arc_to({ x, y - radius }, radius, false, true);
        path.arc_to({ x + radius, y }, radius, false, true);
        path.close();
        return path;
    }
    case ShapeState::Default:
        // The shape is a rectangle that exactly covers the entire image.
        return rectangle_path({ 0, 0 }, { static_cast<float>(image_size.width().to_double()), static_cast<float>(image_size.height().to_double()) });
    case ShapeState::Polygon: {
        if (coords.size() < 6)
            return {};

        Gfx::Path path;
        path.move_to(vertex(0));
        for (size_t i = 1; i < coords.size() / 2; ++i)
            path.line_to(vertex(i));
        path.close();
        return path;
    }
    case ShapeState::Rectangle: {
        if (coords.size() < 4)
            return {};

        // 5. If the shape attribute represents the rectangle state, and the first number in the list is numerically
        //    greater than the third number in the list, then swap those two numbers around.
        if (coords[0] > coords[2])
            swap(coords[0], coords[2]);

        // 6. If the shape attribute represents the rectangle state, and the second number in the list is
        //    numerically greater than the fourth number in the list, then swap those two numbers around.
        if (coords[1] > coords[3])
            swap(coords[1], coords[3]);

        // The shape is a rectangle whose top-left corner is given by the coordinate (x1, y1) and whose bottom right
        // corner is given by the coordinate (x2, y2), those coordinates being interpreted as CSS pixels from the
        // top left corner of the image.
        return rectangle_path(vertex(0), vertex(1));
    }
    }
    VERIFY_NOT_REACHED();
}

bool HTMLAreaElement::shape_contains_point(CSSPixelPoint point, CSSPixelSize image_size) const
{
    // AD-HOC: The coordinates of a shape are interpreted relative to the displayed image, so the rectangle that
    //         exactly covers the entire image excludes the image's borders and padding. Treat the default state as
    //         covering everything that hits the image instead, matching the behavior of other engines.
    if (shape_state() == ShapeState::Default)
        return true;

    auto path = shape_path(image_size);
    if (!path.has_value())
        return false;
    return path->contains(point.to_type<float>(), Gfx::WindingRule::EvenOdd);
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
