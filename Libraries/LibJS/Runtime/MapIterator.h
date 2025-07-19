/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class MapIterator final : public Object
    , public BuiltinIterator {
    JS_OBJECT(MapIterator, Object);
    GC_DECLARE_ALLOCATOR(MapIterator);

public:
    static GC::Ref<MapIterator> create(Realm&, Map& map, Object::PropertyKind iteration_kind);

    virtual ~MapIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined(IteratorRecord const&) override;
    ThrowCompletionOr<void> next(VM&, bool& done, Value& value) override;

private:
    friend class MapIteratorPrototype;

    explicit MapIterator(Map& map, Object::PropertyKind iteration_kind, Object& prototype);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Map> m_map;
    bool m_done { false };
    Object::PropertyKind m_iteration_kind;
    Map::ConstIterator m_iterator;
};

}
