/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <LibGC/Forward.h>
#include <LibGC/Weak.h>

namespace GC {

class GC_API WeakBlock {
public:
    static constexpr size_t BLOCK_SIZE = 16 * KiB;

    static WeakBlock* create();
    static void destroy(WeakBlock*);

    WeakImpl* allocate(Cell*);
    void deallocate(WeakImpl*);

    bool can_allocate() const { return m_freelist != nullptr; }
    bool has_referenced_impls() const;

    void sweep();

    // Points every weak pointer at the cell that its cell moved to, which forwarding_address returns, or null if the
    // cell did not move.
    template<typename Callback>
    void update_moved_cells(Callback forwarding_address)
    {
        for (auto& impl : m_impls) {
            if (impl.state() != WeakImpl::State::Allocated || !impl.ptr())
                continue;
            if (auto* moved_to = forwarding_address(impl.ptr()))
                impl.set_ptr({}, moved_to);
        }
    }

private:
    WeakBlock();
    ~WeakBlock();

    IntrusiveListNode<WeakBlock> m_list_node;

public:
    using List = IntrusiveList<&WeakBlock::m_list_node>;

    WeakImpl* m_freelist { nullptr };

    static constexpr size_t IMPL_COUNT = (BLOCK_SIZE - sizeof(m_list_node) - sizeof(WeakImpl*)) / sizeof(WeakImpl);
    WeakImpl m_impls[IMPL_COUNT];
};

static_assert(sizeof(WeakBlock) <= WeakBlock::BLOCK_SIZE);

}
