/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// https://drafts.csswg.org/css-anchor-position-1/#typedef-anchor-name
class AnchorNameMap {
public:
    void register_name(FlyString const& name, GC::Ref<Element>);
    void unregister_name(FlyString const& name, GC::Ref<Element>);
    GC::Ptr<Element> element_by_name(FlyString const& name) const;

    template<typename Visitor>
    void visit_edges(Visitor& visitor) { visitor.visit(m_map); }

private:
    HashMap<FlyString, Vector<GC::Ref<Element>>> m_map;
};

}
