/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// https://drafts.csswg.org/css-anchor-position-1/#typedef-anchor-name
class AnchorNameMap {
public:
    void register_name(Utf16FlyString const& name, GC::Ref<Element>);
    void unregister_name(Utf16FlyString const& name, GC::Ref<Element>);
    GC::Ptr<Element> last_element_by_name_matching(Utf16FlyString const& name, Function<bool(Element&)> const& is_acceptable) const;

    [[nodiscard]] bool has_registered_names() const { return !m_map.is_empty(); }

    template<typename Visitor>
    void visit_edges(Visitor& visitor) { visitor.visit(m_map); }

private:
    HashMap<Utf16FlyString, Vector<GC::Ref<Element>>> m_map;
};

}
