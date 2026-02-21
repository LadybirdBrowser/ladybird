/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ElementByIdMap.h>

namespace Web::DOM {

void ElementByIdMap::add(FlyString const& element_id, Element& element)
{
    auto& entry = m_map.ensure(element_id, [] { return MapEntry {}; });

    for (auto const& existing : entry.elements) {
        if (existing.ptr() == &element)
            return;
    }

    entry.elements.append(element);

    if (entry.elements.size() == 1) {
        entry.cached_first_element = element;
    } else {
        entry.cached_first_element = {};
    }
}

void ElementByIdMap::remove(FlyString const& element_id, Element& element)
{
    auto it = m_map.find(element_id);
    if (it == m_map.end())
        return;
    auto& entry = it->value;

    entry.elements.remove_all_matching([&](auto& weak_el) {
        return !weak_el || weak_el.ptr() == &element;
    });

    if (entry.elements.is_empty()) {
        m_map.remove(it);
        return;
    }

    if (entry.cached_first_element.ptr() == &element)
        entry.cached_first_element = {};
}

GC::Ptr<Element> ElementByIdMap::get(FlyString const& element_id, Node const& scope_root) const
{
    auto maybe_entry = m_map.get(element_id);
    if (!maybe_entry.has_value())
        return {};
    auto& entry = *maybe_entry;

    if (auto element = entry.cached_first_element)
        return *element;

    GC::Ptr<Element> first_element;
    scope_root.for_each_in_inclusive_subtree_of_type<Element>([&](Element const& el) {
        if (el.id() == element_id) {
            first_element = const_cast<Element*>(&el);
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });

    if (first_element)
        entry.cached_first_element = *first_element;

    return first_element;
}

void ElementByIdMap::for_each_element_with_id(StringView element_id, Node const& scope_root, Function<void(Element&)> callback) const
{
    auto maybe_entry = m_map.get(element_id);
    if (!maybe_entry.has_value())
        return;
    auto& entry = *maybe_entry;

    if (entry.elements.size() == 1) {
        if (auto element = entry.elements[0])
            callback(*element);
        return;
    }

    scope_root.for_each_in_inclusive_subtree_of_type<Element>([&](Element const& el) {
        if (el.id() == element_id)
            callback(const_cast<Element&>(el));
        return TraversalDecision::Continue;
    });
}

}
