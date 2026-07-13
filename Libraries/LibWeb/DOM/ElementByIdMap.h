/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Utf16View.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class ElementByIdMap {
public:
    void add(Utf16FlyString const& element_id, Element&);
    void remove(Utf16FlyString const& element_id, Element&);
    GC::Ptr<Element> get(Utf16View element_id, Node const& scope_root) const;
    void for_each_element_with_id(Utf16View element_id, Node const& scope_root, Function<void(Element&)> callback) const;

    template<typename Callback>
    void for_each_id(Callback callback)
    {
        for (auto const& id : m_map.keys())
            callback(id);
    }

private:
    struct MapEntry {
        GC::Weak<Element> cached_first_element;
        Vector<GC::Weak<Element>> elements;
    };
    mutable HashMap<Utf16FlyString, MapEntry> m_map;
};

}
