/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ConservativeHashMap.h>
#include <LibGC/Heap.h>

namespace GC {

ConservativeHashMapBase::ConservativeHashMapBase()
    : ConservativeHashMapBase(Heap::the())
{
}

ConservativeHashMapBase::ConservativeHashMapBase(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_conservative_hash_map({}, *this);
}

ConservativeHashMapBase::~ConservativeHashMapBase()
{
    m_heap->did_destroy_conservative_hash_map({}, *this);
}

}
