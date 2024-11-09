/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLFieldSetElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLOutputElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLFieldSetElement);

HTMLFieldSetElement::HTMLFieldSetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLFieldSetElement::~HTMLFieldSetElement() = default;

void HTMLFieldSetElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFieldSetElement);
}

void HTMLFieldSetElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_elements);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-fieldset-disabled
bool HTMLFieldSetElement::is_disabled() const
{
    // A fieldset element is a disabled fieldset if it matches any of the following conditions:
    // - Its disabled attribute is specified
    if (has_attribute(AttributeNames::disabled))
        return true;

    // - It is a descendant of another fieldset element whose disabled attribute is specified, and is not a descendant of that fieldset element's first legend element child, if any.
    for (auto* fieldset_ancestor = first_ancestor_of_type<HTMLFieldSetElement>(); fieldset_ancestor; fieldset_ancestor = fieldset_ancestor->first_ancestor_of_type<HTMLFieldSetElement>()) {
        if (fieldset_ancestor->has_attribute(HTML::AttributeNames::disabled)) {
            auto* first_legend_element_child = fieldset_ancestor->first_child_of_type<HTMLLegendElement>();
            if (!first_legend_element_child || !is_descendant_of(*first_legend_element_child))
                return true;
        }
    }

    return false;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-fieldset-elements
JS::GCPtr<DOM::HTMLCollection> const& HTMLFieldSetElement::elements()
{
    // The elements IDL attribute must return an HTMLCollection rooted at the fieldset element, whose filter matches listed elements.
    if (!m_elements) {
        m_elements = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](DOM::Element const& element) {
            // FIXME: Form-associated custom elements return also true
            return is<HTMLButtonElement>(element)
                || is<HTMLFieldSetElement>(element)
                || is<HTMLInputElement>(element)
                || is<HTMLObjectElement>(element)
                || is<HTMLOutputElement>(element)
                || is<HTMLSelectElement>(element)
                || is<HTMLTextAreaElement>(element);
        });
    }
    return m_elements;
}

}
