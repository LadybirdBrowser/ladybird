/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ConservativeRangeProvider.h>
#include <LibGC/Heap.h>

namespace GC {

ConservativeRangeProvider::ConservativeRangeProvider(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_conservative_range_provider({}, *this);
}

ConservativeRangeProvider::~ConservativeRangeProvider()
{
    if (m_heap)
        m_heap->did_destroy_conservative_range_provider({}, *this);
}

}
