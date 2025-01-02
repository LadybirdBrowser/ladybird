/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/RootVector.h>

namespace GC {

RootVectorBase::RootVectorBase(Heap& heap)
    : m_heap(&heap)
{
    m_heap->did_create_root_vector({}, *this);
}

RootVectorBase::~RootVectorBase()
{
    m_heap->did_destroy_root_vector({}, *this);
}

RootVectorBase& RootVectorBase::operator=(RootVectorBase const& other)
{
    if (m_heap != other.m_heap) {
        m_heap = other.m_heap;

        // NOTE: IntrusiveList will remove this RootVectorBase from the old heap it was part of.
        m_heap->did_create_root_vector({}, *this);
    }

    return *this;
}

}
