/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/WeakContainer.h>

namespace GC {

WeakContainer::WeakContainer(Heap& heap)
    : m_heap(heap)
{
    m_heap.did_create_weak_container({}, *this);
}

WeakContainer::~WeakContainer()
{
    deregister();
}

void WeakContainer::deregister()
{
    if (!m_registered)
        return;
    m_heap.did_destroy_weak_container({}, *this);
    m_registered = false;
}

}
