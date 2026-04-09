/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ConservativeHashTable.h>
#include <LibGC/Heap.h>

namespace GC {

ConservativeHashTableBase::ConservativeHashTableBase()
    : ConservativeHashTableBase(Heap::the())
{
}

ConservativeHashTableBase::ConservativeHashTableBase(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_conservative_hash_table({}, *this);
}

ConservativeHashTableBase::~ConservativeHashTableBase()
{
    m_heap->did_destroy_conservative_hash_table({}, *this);
}

}
