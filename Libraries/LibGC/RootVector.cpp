/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MainThreadAssertions.h>
#include <LibGC/Heap.h>
#include <LibGC/RootVector.h>

namespace GC {

RootVectorBase::RootVectorBase(Heap& heap)
    : m_heap(&heap)
{
    ASSERT_ON_MAIN_THREAD();
    m_heap->did_create_root_vector({}, *this);
}

RootVectorBase::~RootVectorBase()
{
    ASSERT_ON_MAIN_THREAD();

    m_heap->did_destroy_root_vector({}, *this);
}

void RootVectorBase::assign_heap(Heap* heap)
{
    if (m_heap == heap)
        return;

    m_heap = heap;

    // NOTE: IntrusiveList will remove this RootVectorBase from the old heap it was part of.
    m_heap->did_create_root_vector({}, *this);
}

}
