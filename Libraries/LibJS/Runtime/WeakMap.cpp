/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/WeakMap.h>

namespace JS {

GC_DEFINE_ALLOCATOR(WeakMap);

GC::Ref<WeakMap> WeakMap::create(Realm& realm)
{
    return realm.create<WeakMap>(realm.intrinsics().weak_map_prototype());
}

WeakMap::WeakMap(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , WeakContainer(heap())
{
}

Optional<Value> WeakMap::weak_map_get(GC::Ptr<Cell> key) const
{
    if (auto it = m_values.find(key); it != m_values.end())
        return it->value;
    return {};
}

bool WeakMap::weak_map_has(GC::Ptr<Cell> key) const
{
    return m_values.contains(key);
}

void WeakMap::weak_map_set(GC::Ptr<Cell> key, Value value)
{
    auto old_external_memory_size = external_memory_size();
    m_values.set(key, value);
    account_external_memory_change(old_external_memory_size);
}

bool WeakMap::weak_map_remove(GC::Ptr<Cell> key)
{
    auto old_external_memory_size = external_memory_size();
    auto did_remove = m_values.remove(key);
    if (did_remove)
        account_external_memory_change(old_external_memory_size);
    return did_remove;
}

void WeakMap::remove_dead_cells(Badge<GC::Heap>)
{
    m_values.remove_all_matching([](Cell* key, Value) {
        return key->state() != Cell::State::Live;
    });
}

size_t WeakMap::external_memory_size() const
{
    return saturating_add_external_memory_size(Object::external_memory_size(), hash_map_external_memory_size(m_values));
}

void WeakMap::account_external_memory_change(size_t old_external_memory_size)
{
    auto new_external_memory_size = external_memory_size();
    if (new_external_memory_size > old_external_memory_size)
        heap().did_allocate_external_memory(new_external_memory_size - old_external_memory_size);
    else if (old_external_memory_size > new_external_memory_size)
        heap().did_free_external_memory(old_external_memory_size - new_external_memory_size);
}

void WeakMap::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_values)
        visitor.visit(entry.value);
}

}
