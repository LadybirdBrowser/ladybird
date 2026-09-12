/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibGC/Cell.h>
#include <LibGC/WeakBlock.h>

namespace GC {

WeakImpl WeakImpl::the_null_weak_impl;

WeakBlock* WeakBlock::create()
{
    auto* block = MUST(Core::System::allocate_anonymous_memory(WeakBlock::BLOCK_SIZE, Core::System::MemoryTag::GarbageCollector));
    return new (block) WeakBlock;
}

WeakBlock::WeakBlock()
{
    for (size_t i = 0; i < IMPL_COUNT; ++i) {
        m_impls[i].set_ptr({}, i + 1 < IMPL_COUNT ? &m_impls[i + 1] : nullptr);
        m_impls[i].set_state(WeakImpl::State::Freelist);
    }
    m_freelist = &m_impls[0];
}

WeakBlock::~WeakBlock() = default;

WeakImpl* WeakBlock::allocate(Cell* cell)
{
    auto* impl = m_freelist;
    if (!impl)
        return nullptr;
    VERIFY(impl->ref_count() == 0);
    m_freelist = impl->ptr() ? static_cast<WeakImpl*>(impl->ptr()) : nullptr;
    impl->set_ptr({}, cell);
    impl->set_state(WeakImpl::State::Allocated);
    return impl;
}

void WeakBlock::deallocate(WeakImpl* impl)
{
    VERIFY(impl->ref_count() == 0);
    impl->set_ptr({}, m_freelist);
    impl->set_state(WeakImpl::State::Freelist);
    m_freelist = impl;
}

void WeakBlock::sweep()
{
    for (size_t i = 0; i < IMPL_COUNT; ++i) {
        auto& impl = m_impls[i];
        if (impl.state() == WeakImpl::State::Freelist)
            continue;
        auto* cell = static_cast<Cell*>(impl.ptr());
        if (!cell || !cell->is_marked())
            impl.set_ptr({}, nullptr);
        if (impl.ref_count() == 0)
            deallocate(&impl);
    }
}

}
