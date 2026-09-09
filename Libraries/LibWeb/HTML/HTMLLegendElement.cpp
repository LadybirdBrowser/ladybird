/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLLegendElement);

HTMLLegendElement::HTMLLegendElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLLegendElement::~HTMLLegendElement() = default;

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-legend-form
HTMLFormElement* HTMLLegendElement::form()
{
    // The form IDL attribute's behavior depends on whether the legend element is in a fieldset element or not.
    // If the legend has a fieldset element as its parent, then the form IDL attribute must return the same value as the form IDL attribute on that fieldset element.
    if (auto* field_set = as_if<HTML::HTMLFieldSetElement>(parent_element().ptr())) {
        return field_set->form();
    }

    // Otherwise, it must return null.
    return nullptr;
}

Layout::Node* HTMLLegendElement::create_layout_node(CSS::LayoutStyle style)
{
    return &Layout::allocate_layout_node<Layout::BlockContainer>(document(), *this, style, Layout::RustFFI::NodeKind::LegendBox);
}

static void refresh_disabled_state_of_form_controls_under_disabled_field_set(DOM::Node* node)
{
    if (auto* field_set = as_if<HTMLFieldSetElement>(node); field_set && field_set->has_attribute(HTML::AttributeNames::disabled))
        field_set->refresh_disabled_state_of_descendant_form_controls();
}

void HTMLLegendElement::inserted()
{
    Base::inserted();
    refresh_disabled_state_of_form_controls_under_disabled_field_set(parent_element().ptr());
}

void HTMLLegendElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<DOM::Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);
    if (is_subtree_root != IsSubtreeRoot::Yes)
        return;
    refresh_disabled_state_of_form_controls_under_disabled_field_set(old_ancestor.ptr());
    if (parent_element().ptr() != old_ancestor.ptr())
        refresh_disabled_state_of_form_controls_under_disabled_field_set(parent_element().ptr());
}

}
