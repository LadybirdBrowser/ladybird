/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/RootHashMap.h>

namespace GC {

RootHashMapBase::RootHashMapBase(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_root_hash_map({}, *this);
}

RootHashMapBase::~RootHashMapBase()
{
    m_heap->did_destroy_root_hash_map({}, *this);
}

void RootHashMapBase::assign_heap(Heap* heap)
{
    if (m_heap == heap)
        return;

    m_heap = heap;

    // NOTE: IntrusiveList will remove this RootHashMap from the old heap it was part of.
    m_heap->did_create_root_hash_map({}, *this);
}

}
