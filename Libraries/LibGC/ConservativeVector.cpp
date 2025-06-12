/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>

namespace GC {

ConservativeVectorBase::ConservativeVectorBase(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_conservative_vector({}, *this);
}

ConservativeVectorBase::~ConservativeVectorBase()
{
    m_heap->did_destroy_conservative_vector({}, *this);
}

}
