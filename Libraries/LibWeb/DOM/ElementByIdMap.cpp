/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ElementByIdMap.h>

namespace Web::DOM {

void ElementByIdMap::add(FlyString const& element_id, Element& element)
{
    auto& elements_with_id = m_map.ensure(element_id, [] { return Vector<WeakPtr<Element>> {}; });

    // Remove all elements that were deallocated.
    elements_with_id.remove_all_matching([](WeakPtr<Element>& element) {
        return !element.has_value();
    });

    elements_with_id.remove_first_matching([&](auto const& another_element) {
        return &element == another_element.ptr();
    });
    elements_with_id.insert_before_matching(element, [&](auto& another_element) {
        return element.is_before(*another_element);
    });
}

void ElementByIdMap::remove(FlyString const& element_id, Element& element)
{
    auto maybe_elements_with_id = m_map.get(element_id);
    if (!maybe_elements_with_id.has_value())
        return;
    auto& elements_with_id = *maybe_elements_with_id;
    elements_with_id.remove_all_matching([&](auto& another_element) {
        if (!another_element.has_value())
            return true;
        return &element == another_element.ptr();
    });
}

}
