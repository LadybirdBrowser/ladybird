/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLMapElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLMapElement);

HTMLMapElement::HTMLMapElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMapElement::~HTMLMapElement() = default;

void HTMLMapElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLMapElement);
    Base::initialize(realm);
}

void HTMLMapElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_areas);
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

// Iterates through a maps associated areas, activating the first element seen in reverse tree order.
void HTMLMapElement::activate_area_by_point(CSSPixels x, CSSPixels y, Web::DOM::Event const& event)
{
    Gfx::IntPoint point_coordinates { x.to_int(), y.to_int() };

    auto area_collection = areas();
    for (size_t i = 0; i < area_collection->length(); ++i) {
        auto* element = area_collection->item(i);
        if (!element || !is<HTMLAreaElement>(*element))
            continue;

        auto& area = static_cast<HTMLAreaElement&>(*element);

        if (area.check_if_contains_point(point_coordinates)) {
            area.activate(event);
            return;
        }
    }
}

}
