/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CrossHeapMember.h>
#include <LibGC/Heap.h>

namespace GC {

void CrossHeapMemberBase::reset(Cell* cell)
{
    auto* new_heap = cell ? &HeapBlockBase::from_cell(cell)->heap() : nullptr;
    if (m_registered_heap == new_heap) {
        m_cell = cell;
        return;
    }

    if (m_registered_heap)
        m_registered_heap->did_destroy_cross_heap_member({}, *this);
    m_cell = cell;
    m_registered_heap = new_heap;
    if (m_registered_heap)
        m_registered_heap->did_create_cross_heap_member({}, *this);
}

}
