/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class ElementByIdMap {
public:
    void add(FlyString const& element_id, Element&);
    void remove(FlyString const& element_id, Element&);
    GC::Ptr<Element> get(FlyString const& element_id, Node const& scope_root) const;
    void for_each_element_with_id(StringView element_id, Node const& scope_root, Function<void(Element&)> callback) const;

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
    mutable HashMap<FlyString, MapEntry> m_map;
};

}
