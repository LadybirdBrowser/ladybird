/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/HeapBlock.h>
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

void WeakSet::remove_dead_cells(Badge<GC::Heap>)
{
    m_values.remove_all_matching([this](Cell* cell) {
        auto* block = GC::HeapBlock::from_cell(cell);
        return !heap().is_live_heap_block(block) || cell->state() != Cell::State::Live;
    });
}

}
