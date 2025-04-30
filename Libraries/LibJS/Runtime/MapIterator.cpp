/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/MapIterator.h>

namespace JS {

GC_DEFINE_ALLOCATOR(MapIterator);

GC::Ref<MapIterator> MapIterator::create(Realm& realm, Map& map, Object::PropertyKind iteration_kind)
{
    return realm.create<MapIterator>(map, iteration_kind, realm.intrinsics().map_iterator_prototype());
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

ThrowCompletionOr<void> MapIterator::next(VM& vm, bool& done, Value& value)
{
    if (m_done) {
        done = true;
        value = js_undefined();
        return {};
    }

    if (m_iterator.is_end()) {
        m_done = true;
        done = true;
        value = js_undefined();
        return {};
    }

    auto entry = *m_iterator;
    ++m_iterator;
    if (m_iteration_kind == Object::PropertyKind::Key) {
        value = entry.key;
        return {};
    }
    if (m_iteration_kind == Object::PropertyKind::Value) {
        value = entry.value;
        return {};
    }

    value = Array::create_from(*vm.current_realm(), { entry.key, entry.value });
    return {};
}

}
