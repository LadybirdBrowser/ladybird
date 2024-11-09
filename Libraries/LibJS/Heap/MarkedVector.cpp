/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Heap/MarkedVector.h>

namespace JS {

MarkedVectorBase::MarkedVectorBase(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_marked_vector({}, *this);
}

MarkedVectorBase::~MarkedVectorBase()
{
    m_heap->did_destroy_marked_vector({}, *this);
}

MarkedVectorBase& MarkedVectorBase::operator=(MarkedVectorBase const& other)
{
    if (m_heap != other.m_heap) {
        m_heap = other.m_heap;

        // NOTE: IntrusiveList will remove this MarkedVectorBase from the old heap it was part of.
        m_heap->did_create_marked_vector({}, *this);
    }

    return *this;
}

}
