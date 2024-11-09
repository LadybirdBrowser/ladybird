/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/WeakRef.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakRef);

NonnullGCPtr<WeakRef> WeakRef::create(Realm& realm, Object& value)
{
    return realm.heap().allocate<WeakRef>(realm, value, realm.intrinsics().weak_ref_prototype());
}

NonnullGCPtr<WeakRef> WeakRef::create(Realm& realm, Symbol& value)
{
    return realm.heap().allocate<WeakRef>(realm, value, realm.intrinsics().weak_ref_prototype());
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

void WeakRef::remove_dead_cells(Badge<Heap>)
{
    if (m_value.visit([](Cell* cell) -> bool { return cell->state() == Cell::State::Live; }, [](Empty) -> bool { VERIFY_NOT_REACHED(); }))
        return;

    m_value = Empty {};
    // This is an optimization, we deregister from the garbage collector early (even if we were not garbage collected ourself yet)
    // to reduce the garbage collection overhead, which we can do because a cleared weak ref cannot be reused.
    WeakContainer::deregister();
}

void WeakRef::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    if (vm().execution_generation() == m_last_execution_generation) {
        auto* cell = m_value.visit([](Cell* cell) -> Cell* { return cell; }, [](Empty) -> Cell* { return nullptr; });
        visitor.visit(cell);
    }
}

}
