/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/RootHashTable.h>

namespace GC {

RootHashTableBase::RootHashTableBase()
    : RootHashTableBase(Heap::the())
{
}

RootHashTableBase::RootHashTableBase(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_root_hash_table({}, *this);
}

RootHashTableBase::~RootHashTableBase()
{
    m_heap->did_destroy_root_hash_table({}, *this);
}

void RootHashTableBase::assign_heap(Heap* heap)
{
    if (m_heap == heap)
        return;

    m_heap = heap;

    // NOTE: IntrusiveList will remove this RootHashTable from the old heap it was part of.
    m_heap->did_create_root_hash_table({}, *this);
}

}
