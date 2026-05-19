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

}
