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

private:
    HashMap<FlyString, Vector<WeakPtr<Element>>> m_map;
};

}
