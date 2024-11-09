/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class MapIterator final : public Object {
    JS_OBJECT(MapIterator, Object);
    JS_DECLARE_ALLOCATOR(MapIterator);

public:
    static NonnullGCPtr<MapIterator> create(Realm&, Map& map, Object::PropertyKind iteration_kind);

    virtual ~MapIterator() override = default;

    Map& map() const { return m_map; }
    bool done() const { return m_done; }
    Object::PropertyKind iteration_kind() const { return m_iteration_kind; }

private:
    friend class MapIteratorPrototype;

    explicit MapIterator(Map& map, Object::PropertyKind iteration_kind, Object& prototype);

    virtual void visit_edges(Cell::Visitor&) override;

    NonnullGCPtr<Map> m_map;
    bool m_done { false };
    Object::PropertyKind m_iteration_kind;
    Map::ConstIterator m_iterator;
};

}
