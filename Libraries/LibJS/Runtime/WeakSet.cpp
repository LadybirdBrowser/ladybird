/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/WeakSet.h>

namespace JS {

GC_DEFINE_ALLOCATOR(WeakSet);

GC::Ref<WeakSet> WeakSet::create(Realm& realm)
{
    return realm.create<WeakSet>(realm.intrinsics().weak_set_prototype());
}

WeakSet::WeakSet(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , WeakContainer(heap())
{
}

bool WeakSet::weak_set_has(GC::Ptr<Cell> value) const
{
    return m_values.contains(value);
}

void WeakSet::weak_set_add(GC::Ptr<Cell> value)
{
    auto old_external_memory_size = external_memory_size();
    m_values.set(value, AK::HashSetExistingEntryBehavior::Keep);
    account_external_memory_change(old_external_memory_size);
}

bool WeakSet::weak_set_remove(GC::Ptr<Cell> value)
{
    auto old_external_memory_size = external_memory_size();
    auto did_remove = m_values.remove(value);
    if (did_remove)
        account_external_memory_change(old_external_memory_size);
    return did_remove;
}

void WeakSet::remove_dead_cells(Badge<GC::Heap>)
{
    m_values.remove_all_matching([](Cell* cell) {
        return cell->state() != Cell::State::Live;
    });
}

size_t WeakSet::external_memory_size() const
{
    return saturating_add_external_memory_size(Object::external_memory_size(), hash_table_external_memory_size(m_values));
}

void WeakSet::account_external_memory_change(size_t old_external_memory_size)
{
    auto new_external_memory_size = external_memory_size();
    if (new_external_memory_size > old_external_memory_size)
        heap().did_allocate_external_memory(new_external_memory_size - old_external_memory_size);
    else if (old_external_memory_size > new_external_memory_size)
        heap().did_free_external_memory(old_external_memory_size - new_external_memory_size);
}

}
