/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLDataListElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLDataListElement);

HTMLDataListElement::HTMLDataListElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLDataListElement::~HTMLDataListElement() = default;

void HTMLDataListElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLDataListElement);
}

void HTMLDataListElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_options);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-datalist-options
JS::NonnullGCPtr<DOM::HTMLCollection> HTMLDataListElement::options()
{
    // The options IDL attribute must return an HTMLCollection rooted at the datalist node, whose filter matches option elements.
    if (!m_options) {
        m_options = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](Element const& element) {
            return is<HTML::HTMLOptionElement>(element);
        });
    }
    return *m_options;
}

}
