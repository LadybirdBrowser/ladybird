/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapAccess.h>
#include <LibGC/Root.h>

namespace GC {

RootImpl::RootImpl(Cell* cell, SourceLocation location)
    : m_cell(cell)
    , m_location(location)
{
    ASSERT(!heap_access_is_forbidden_on_this_thread());
    m_cell->heap().did_create_root({}, *this);
}

RootImpl::~RootImpl()
{
    ASSERT(!heap_access_is_forbidden_on_this_thread());
    m_cell->heap().did_destroy_root({}, *this);
}

}
