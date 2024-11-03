/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/ConservativeVector.h>
#include <LibJS/Heap/Heap.h>

namespace JS {

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
