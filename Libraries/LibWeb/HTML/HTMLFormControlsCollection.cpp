/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/HTMLFormControlsCollection.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/RadioNodeList.h>

namespace Web::HTML {

static Utf16View radio_node_list_name_view(Utf16String const& name)
{
    return name.utf16_view();
}

static bool radio_node_list_element_matches_name(DOM::Element const& element, Utf16View name)
{
    return (element.id().has_value() && element.id()->view() == name)
        || (element.name().has_value() && element.name()->view() == name);
}

GC_DEFINE_ALLOCATOR(HTMLFormControlsCollection);

GC::Ref<HTMLFormControlsCollection> HTMLFormControlsCollection::create(DOM::ParentNode& root, Scope scope, HTMLFormElement& form, Function<bool(DOM::Element const&)> filter)
{
    return GC::Heap::the().allocate<HTMLFormControlsCollection>(root, scope, form, move(filter));
}

HTMLFormControlsCollection::HTMLFormControlsCollection(DOM::ParentNode& root, Scope scope, HTMLFormElement& form, Function<bool(DOM::Element const&)> filter)
    : DOM::HTMLCollection(root, scope, move(filter), AttributeInvalidationType::FormControls, nullptr, Kind::FormControls)
    , m_form(form)
{
}

HTMLFormControlsCollection::~HTMLFormControlsCollection() = default;

void HTMLFormControlsCollection::collect_elements_into(Vector<GC::RawPtr<DOM::Element>>& elements) const
{
    // OPTIMIZATION: A form keeps its associated elements in tree order, so the collection is filtered from those
    //               rather than from a walk of the whole tree the form is in.
    for (auto const& element : m_form->associated_elements_in_tree_order({})) {
        if (element_matches_filter(*element))
            elements.append(*element);
    }
}

void HTMLFormControlsCollection::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_form);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmlformcontrolscollection-nameditem
Variant<Empty, GC::Ref<DOM::Element>, GC::Ref<RadioNodeList>> HTMLFormControlsCollection::named_item_or_radio_node_list(Utf16View name) const
{
    // 1. If name is the empty string, return null and stop the algorithm.
    if (name.is_empty())
        return {};

    // 2. If, at the time the method is called, there is exactly one node in the collection that has either an id attribute or a name attribute equal to name, then return that node and stop the algorithm.
    // 3. Otherwise, if there are no nodes in the collection that have either an id attribute or a name attribute equal to name, then return null and stop the algorithm.
    bool multiple_matching = false;
    DOM::Element* matching_element = nullptr;
    for (auto const& element : collect_matching_elements()) {
        bool id_matches = element->id().has_value() && element->id()->view() == name;
        bool name_matches = element->name().has_value() && element->name()->view() == name;
        if (!id_matches && !name_matches)
            continue;
        if (matching_element) {
            multiple_matching = true;
            break;
        }
        matching_element = element.ptr();
    }

    if (!matching_element)
        return {};

    if (!multiple_matching)
        return GC::Ref { *matching_element };

    // 4. Otherwise, create a new RadioNodeList object representing a live view of the HTMLFormControlsCollection object, further filtered so that the only nodes in the
    //    RadioNodeList object are those that have either an id attribute or a name attribute equal to name. The nodes in the RadioNodeList object must be sorted in tree
    //    order. Return that RadioNodeList object.
    auto name_copy = Utf16String::from_utf16(name);
    auto filter = [collection = GC::Ref { *this }, name = move(name_copy)](auto const& node) {
        if (!is<DOM::Element>(node))
            return false;

        auto const& element = as<DOM::Element>(node);
        return collection->element_matches_filter(element)
            && radio_node_list_element_matches_name(element, radio_node_list_name_view(name));
    };
    return RadioNodeList::create(root(), DOM::LiveNodeList::Scope::Descendants, move(filter), DOM::LiveNodeList::Kind::FormControls);
}

}
