/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/HeapBlock.h>
#include <LibJS/Runtime/WeakRef.h>

namespace JS {

GC_DEFINE_ALLOCATOR(WeakRef);

GC::Ref<WeakRef> WeakRef::create(Realm& realm, Object& value)
{
    return realm.create<WeakRef>(value, realm.intrinsics().weak_ref_prototype());
}

GC::Ref<WeakRef> WeakRef::create(Realm& realm, Symbol& value)
{
    return realm.create<WeakRef>(value, realm.intrinsics().weak_ref_prototype());
}

WeakRef::WeakRef(Object& value, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , WeakContainer(heap())
    , m_value(&value)
    , m_last_execution_generation(vm().execution_generation())
{
}

WeakRef::WeakRef(Symbol& value, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , WeakContainer(heap())
    , m_value(&value)
    , m_last_execution_generation(vm().execution_generation())
{
}

void WeakRef::remove_dead_cells(Badge<GC::Heap>)
{
    auto is_alive = m_value.visit(
        [this]<typename T>(GC::Ptr<T> cell) -> bool {
            auto* block = GC::HeapBlock::from_cell(cell.ptr());
            return heap().is_live_heap_block(block) && cell->state() == Cell::State::Live && cell->is_marked();
        },
        [](Empty) -> bool { return true; });
    if (is_alive)
        return;

    m_value = Empty {};
}

void WeakRef::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    if (vm().execution_generation() == m_last_execution_generation) {
        GC::Ptr<Cell> cell = m_value.visit([]<typename T>(GC::Ptr<T> cell) -> GC::Ptr<Cell> { return cell; }, [](Empty) -> GC::Ptr<Cell> { return nullptr; });
        visitor.visit(cell);
    }
}

}
