/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// https://drafts.csswg.org/css-anchor-position-1/#typedef-anchor-name
//
// The elements a name is registered on are published to the layout arena as they change, because an
// anchor query happens during layout. A map belongs to one tree scope, which the arena names by the
// shadow host of the scope's root, or by nothing for the document tree.
class AnchorNameMap {
public:
    // `scope_host` is empty for the document tree, and otherwise the identity of the shadow host
    // of the map's root.
    void register_name(Utf16FlyString const& name, GC::Ref<Element>, Optional<CSS::StyleNodeID> scope_host);
    void unregister_name(Utf16FlyString const& name, GC::Ref<Element>, Optional<CSS::StyleNodeID> scope_host);
    GC::Ptr<Element> last_element_by_name_matching(Utf16FlyString const& name, Function<bool(Element&)> const& is_acceptable) const;

    template<typename Visitor>
    void visit_edges(Visitor& visitor) { visitor.visit(m_map); }

private:
    void publish(Utf16FlyString const& name, Document&, Optional<CSS::StyleNodeID> scope_host) const;

    HashMap<Utf16FlyString, Vector<GC::Ref<Element>>> m_map;
};

}
