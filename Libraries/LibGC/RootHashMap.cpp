/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MainThreadAssertions.h>
#include <LibGC/Heap.h>
#include <LibGC/RootHashMap.h>

namespace GC {

RootHashMapBase::RootHashMapBase(Heap& heap)
    : m_heap(&heap)
{
    ASSERT_ON_MAIN_THREAD();
    m_heap->did_create_root_hash_map({}, *this);
}

RootHashMapBase::~RootHashMapBase()
{
    ASSERT_ON_MAIN_THREAD();
    m_heap->did_destroy_root_hash_map({}, *this);
}

void RootHashMapBase::assign_heap(Heap* heap)
{
    if (m_heap == heap)
        return;

    m_heap = heap;

    // NOTE: IntrusiveList will remove this RootHashMap from the old heap it was part of.
    ASSERT_ON_MAIN_THREAD();
    m_heap->did_create_root_hash_map({}, *this);
}

}
