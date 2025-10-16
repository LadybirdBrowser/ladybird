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

    WeakImpl* allocate(Cell*);
    void deallocate(WeakImpl*);

    bool can_allocate() const { return m_freelist != nullptr; }

    void sweep();

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
