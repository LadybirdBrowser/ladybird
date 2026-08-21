/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLOutputElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Layout/BlockContainer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFieldSetElement);

HTMLFieldSetElement::HTMLFieldSetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLFieldSetElement::~HTMLFieldSetElement() = default;

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

void HTMLFieldSetElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
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
        auto filter = [](DOM::Element const& element) {
            if (auto const* form_associated_element = as_if<FormAssociatedElement>(element); form_associated_element && form_associated_element->is_listed())
                return true;

            return false;
        };
        m_elements = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, move(filter), DOM::HTMLCollection::AttributeInvalidationType::FormControls, nullptr, DOM::HTMLCollection::Kind::FormControls);
    }
    return m_elements;
}

RefPtr<Layout::Node> HTMLFieldSetElement::create_layout_node(CSS::LayoutStyle style)
{
    auto fieldset_box = make_ref_counted<Layout::BlockContainer>(document(), this, style, Layout::RustFFI::NodeKind::FieldSetBox);
    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    // If the computed outer display type is inline, the fieldset is expected to behave as inline-block. Otherwise, it
    // is expected to behave as flow-root. This does not change the computed value.
    if (fieldset_box->display().is_flow_inside()) {
        fieldset_box->modify_computed_values([&](auto& values) {
            values.set_display(CSS::Display { fieldset_box->display().outside(), CSS::DisplayInside::FlowRoot });
        });
    }
    return fieldset_box;
}

}
