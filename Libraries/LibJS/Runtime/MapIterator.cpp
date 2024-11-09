/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/MapIterator.h>

namespace JS {

JS_DEFINE_ALLOCATOR(MapIterator);

NonnullGCPtr<MapIterator> MapIterator::create(Realm& realm, Map& map, Object::PropertyKind iteration_kind)
{
    return realm.heap().allocate<MapIterator>(realm, map, iteration_kind, realm.intrinsics().map_iterator_prototype());
}

MapIterator::MapIterator(Map& map, Object::PropertyKind iteration_kind, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_map(map)
    , m_iteration_kind(iteration_kind)
    , m_iterator(static_cast<Map const&>(map).begin())
{
}

void MapIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_map);
}

}
