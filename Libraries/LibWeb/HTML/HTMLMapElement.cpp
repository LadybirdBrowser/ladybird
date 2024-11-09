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

JS_DEFINE_ALLOCATOR(HTMLMapElement);

HTMLMapElement::HTMLMapElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMapElement::~HTMLMapElement() = default;

void HTMLMapElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLMapElement);
}

void HTMLMapElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_areas);
}

// https://html.spec.whatwg.org/multipage/image-maps.html#dom-map-areas
JS::NonnullGCPtr<DOM::HTMLCollection> HTMLMapElement::areas()
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
