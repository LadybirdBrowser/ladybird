/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AnchorNameMap.h>
#include <LibWeb/DOM/Element.h>

namespace Web::DOM {

void AnchorNameMap::register_name(FlyString const& name, GC::Ref<Element> element)
{
    auto& elements = m_map.ensure(name);
    if (elements.contains_slow(element))
        return;

    // Insert in tree order so that .last() is always the last element in tree order.
    auto index = elements.find_first_index_if([&](auto& existing) {
        return element->is_before(existing);
    });
    if (index.has_value())
        elements.insert(*index, element);
    else
        elements.append(element);
}

void AnchorNameMap::unregister_name(FlyString const& name, GC::Ref<Element> element)
{
    auto it = m_map.find(name);
    if (it == m_map.end())
        return;
    it->value.remove_first_matching([&](auto& e) { return e == element; });
    if (it->value.is_empty())
        m_map.remove(it);
}

GC::Ptr<Element> AnchorNameMap::element_by_name(FlyString const& name) const
{
    auto it = m_map.find(name);
    if (it == m_map.end() || it->value.is_empty())
        return {};
    return it->value.last();
}

}
