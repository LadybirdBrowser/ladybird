/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLMapElement);

HTMLMapElement::HTMLMapElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMapElement::~HTMLMapElement() = default;

void HTMLMapElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_areas);
}

// https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
GC::Ptr<HTMLAreaElement> HTMLMapElement::area_for_point(CSSPixelPoint point, CSSPixelSize image_size)
{
    // 3. Otherwise, the user agent must collect all the area elements that are descendants of the map. Let areas
    //    be that list.
    // Pointing device interaction with an image associated with a set of layered shapes per the above algorithm
    // must result in the relevant user interaction events being first fired to the top-most shape covering the
    // point that the pointing device indicated, if any, or to the image element itself, if there is no shape
    // covering that point.
    // NB: The shapes are layered in reverse tree order, so the top-most shape covering the point belongs to the
    //     first area element in tree order whose shape contains the point.
    GC::Ptr<HTMLAreaElement> hit_area;
    for_each_in_subtree_of_type<HTMLAreaElement>([&](HTMLAreaElement& area) {
        if (!area.shape_contains_point(point, image_size))
            return TraversalDecision::Continue;
        hit_area = area;
        return TraversalDecision::Break;
    });
    return hit_area;
}

// https://html.spec.whatwg.org/multipage/interaction.html#get-the-focusable-area
GC::Ptr<HTMLImageElement> HTMLMapElement::first_image_with_focusable_shapes() const
{
    // Return the shape corresponding to the first img element in tree order that uses the image map to which the area
    // element belongs.
    return first_associated_image_matching([](HTMLImageElement& image_element) {
        // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
        // The shapes of area elements in an image map associated with an img element that is being rendered and is
        // not inert.
        return image_element.meets_focusable_area_rendering_requirements() && !image_element.is_inert();
    });
}

GC::Ptr<HTMLImageElement> HTMLMapElement::first_painted_image_with_focusable_shapes() const
{
    return first_associated_image_matching([](HTMLImageElement& image_element) {
        return image_element.paintable_box() && !image_element.is_inert();
    });
}

// https://html.spec.whatwg.org/multipage/image-maps.html#dom-map-areas
GC::Ref<DOM::HTMLCollection> HTMLMapElement::areas()
{
    // The areas attribute must return an HTMLCollection rooted at the map element, whose filter matches only area elements.
    if (!m_areas) {
        m_areas = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](Element const& element) {
            return is<HTML::HTMLAreaElement>(element);
        });
    }
    return *m_areas;
}

}
