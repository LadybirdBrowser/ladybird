/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Parser/ListOfActiveFormattingElements.h>

namespace Web::HTML {

static constexpr size_t NoahsArkCapacity = 3;
ListOfActiveFormattingElements::~ListOfActiveFormattingElements() = default;

void ListOfActiveFormattingElements::visit_edges(JS::Cell::Visitor& visitor)
{
    for (auto& entry : m_entries)
        visitor.visit(entry.element);
}

void ListOfActiveFormattingElements::ensure_noahs_ark_clause(DOM::Element& element)
{
    Vector<Entry> possible_matches;
    for (size_t i = m_entries.size(); i > 0;) {
        i--;
        auto& entry = m_entries[i];
        if (entry.is_marker())
            break;
        if (entry.element->local_name() == element.local_name()
            && entry.element->namespace_uri() == element.namespace_uri()
            && entry.element->attribute_list_size() == element.attribute_list_size())
            possible_matches.append(entry);
    }

    if (possible_matches.size() < NoahsArkCapacity)
        return;

    // FIXME: the attributes should be compared as they where created by the parser
    element.for_each_attribute([&](auto& name, auto& value) {
        possible_matches.remove_all_matching([&](auto& entry) {
            auto attr = entry.element->get_attribute(name);
            return !attr.has_value() || attr != value;
        });
    });

    if (possible_matches.size() < NoahsArkCapacity)
        return;

    remove(*possible_matches.last().element);
}

// https://html.spec.whatwg.org/multipage/parsing.html#push-onto-the-list-of-active-formatting-elements
void ListOfActiveFormattingElements::add(DOM::Element& element)
{
    // 1. If there are already three elements in the list of active formatting elements after the last marker, if any, or anywhere in the list if there are no markers,
    //    that have the same tag name, namespace, and attributes as element, then remove the earliest such element from the list of active formatting elements.
    //    For these purposes, the attributes must be compared as they were when the elements were created by the parser; two elements have the same attributes if all their parsed attributes
    //    can be paired such that the two attributes in each pair have identical names, namespaces, and values (the order of the attributes does not matter).
    ensure_noahs_ark_clause(element);
    // 2. Add element to the list of active formatting elements.
    m_entries.append({ element });
}

void ListOfActiveFormattingElements::add_marker()
{
    m_entries.append({ nullptr });
}

bool ListOfActiveFormattingElements::contains(DOM::Element const& element) const
{
    for (auto& entry : m_entries) {
        if (entry.element.ptr() == &element)
            return true;
    }
    return false;
}

DOM::Element* ListOfActiveFormattingElements::last_element_with_tag_name_before_marker(FlyString const& tag_name)
{
    for (ssize_t i = m_entries.size() - 1; i >= 0; --i) {
        auto& entry = m_entries[i];
        if (entry.is_marker())
            return nullptr;
        if (entry.element->local_name() == tag_name)
            return entry.element.ptr();
    }
    return nullptr;
}

void ListOfActiveFormattingElements::remove(DOM::Element& element)
{
    m_entries.remove_first_matching([&](auto& entry) {
        return entry.element.ptr() == &element;
    });
}

void ListOfActiveFormattingElements::clear_up_to_the_last_marker()
{
    while (!m_entries.is_empty()) {
        auto entry = m_entries.take_last();
        if (entry.is_marker())
            break;
    }
}

Optional<size_t> ListOfActiveFormattingElements::find_index(DOM::Element const& element) const
{
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (m_entries[i].element.ptr() == &element)
            return i;
    }
    return {};
}

void ListOfActiveFormattingElements::replace(DOM::Element& to_remove, DOM::Element& to_add)
{
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (m_entries[i].element.ptr() == &to_remove)
            m_entries[i].element = GC::make_root(to_add);
    }
}

void ListOfActiveFormattingElements::insert_at(size_t index, DOM::Element& element)
{
    m_entries.insert(index, { element });
}

}
