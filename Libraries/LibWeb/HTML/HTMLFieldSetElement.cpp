/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLFieldSetElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLOutputElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Layout/FieldSetBox.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFieldSetElement);

HTMLFieldSetElement::HTMLFieldSetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLFieldSetElement::~HTMLFieldSetElement() = default;

void HTMLFieldSetElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFieldSetElement);
    Base::initialize(realm);
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

void HTMLFieldSetElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::disabled) {
        for_each_in_subtree_of_type<HTMLElement>([](auto& element) {
            if (element.is_form_associated_custom_element())
                element.update_face_disabled_state();
            return TraversalDecision::Continue;
        });
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-fieldset-elements
GC::Ptr<DOM::HTMLCollection> const& HTMLFieldSetElement::elements()
{
    // The elements IDL attribute must return an HTMLCollection rooted at the fieldset element, whose filter matches listed elements.
    if (!m_elements) {
        m_elements = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](DOM::Element const& element) {
            if (auto const* form_associated_element = as_if<FormAssociatedElement>(element); form_associated_element && form_associated_element->is_listed())
                return true;

            return false;
        });
    }
    return m_elements;
}

Layout::FieldSetBox* HTMLFieldSetElement::layout_node()
{
    return static_cast<Layout::FieldSetBox*>(Node::layout_node());
}

GC::Ptr<Layout::Node> HTMLFieldSetElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::FieldSetBox>(document(), *this, style);
}

}
