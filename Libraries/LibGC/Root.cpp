/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>

namespace GC {

RootImpl::RootImpl(Cell* cell, SourceLocation location)
    : m_cell(cell)
    , m_location(location)
{
    m_cell->heap().did_create_root({}, *this);
}

RootImpl::~RootImpl()
{
    m_cell->heap().did_destroy_root({}, *this);
}

}
