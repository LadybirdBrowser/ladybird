/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class ElementByIdMap {
public:
    void add(FlyString const& element_id, Element&);
    void remove(FlyString const& element_id, Element&);
    GC::Ptr<Element> get(FlyString const& element_id) const;

    template<typename Callback>
    void for_each_id(Callback callback)
    {
        for (auto const& id : m_map.keys())
            callback(id);
    }

    template<typename Callback>
    void for_each_element_with_id(StringView id, Callback callback)
    {
        auto maybe_elements_with_id = m_map.get(id);
        if (!maybe_elements_with_id.has_value())
            return;
        for (auto const& element : *maybe_elements_with_id) {
            if (element)
                callback(GC::Ref { *element });
        }
    }

private:
    HashMap<FlyString, Vector<GC::Weak<Element>>> m_map;
};

}
