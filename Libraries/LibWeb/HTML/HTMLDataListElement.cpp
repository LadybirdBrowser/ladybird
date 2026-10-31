/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLDataListElement);

HTMLDataListElement::HTMLDataListElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLDataListElement::~HTMLDataListElement() = default;

void HTMLDataListElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_options);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-datalist-options
GC::Ref<DOM::HTMLCollection> HTMLDataListElement::options()
{
    // The options IDL attribute must return an HTMLCollection rooted at the datalist node, whose filter matches option elements.
    if (!m_options) {
        m_options = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](Element const& element) { return is<HTML::HTMLOptionElement>(element); }, DOM::HTMLCollection::AttributeInvalidationType::None);
    }
    return *m_options;
}

void HTMLDataListElement::inserted()
{
    Base::inserted();
    notify_inputs_using_this_as_suggestions_source();
}

void HTMLDataListElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    notify_inputs_using_this_as_suggestions_source();
}

void HTMLDataListElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    notify_inputs_with_suggestions_sources_in(old_root);
}

void HTMLDataListElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (name == AttributeNames::id && old_value != value)
        notify_inputs_with_suggestions_sources_in(root());
}

// NB: An input whose suggestions source element was this datalist, under an id it no longer has or in a tree it has
//     left, shows the suggestions of whatever element its list attribute now names.
void HTMLDataListElement::notify_inputs_with_suggestions_sources_in(DOM::Node& root)
{
    queue_an_element_task(HTML::Task::Source::DOMManipulation, [root = GC::Ref { root }] {
        root->for_each_in_inclusive_subtree_of_type<HTMLInputElement>([](HTMLInputElement& input) {
            if (input.has_attribute(AttributeNames::list))
                input.suggestions_source_element_did_change();
            return TraversalDecision::Continue;
        });
    });
}

// NB: An input that shows the suggestions of a datalist, such as the ticks of a slider, shows them again once the
//     datalist is inserted or its options change. The inputs are told once for any number of changes before the next
//     task, so that parsing a long list of options does not search the document for each of them.
void HTMLDataListElement::notify_inputs_using_this_as_suggestions_source()
{
    if (m_input_notification_queued || !id().has_value())
        return;
    m_input_notification_queued = true;
    queue_an_element_task(HTML::Task::Source::DOMManipulation, [this] {
        m_input_notification_queued = false;
        root().for_each_in_inclusive_subtree_of_type<HTMLInputElement>([&](HTMLInputElement& input) {
            if (auto list = input.get_attribute(AttributeNames::list); list.has_value() && id() == *list)
                input.suggestions_source_element_did_change();
            return TraversalDecision::Continue;
        });
    });
}

}
