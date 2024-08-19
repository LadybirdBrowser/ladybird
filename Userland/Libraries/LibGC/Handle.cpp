/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/Handle.h>
#include <LibGC/Heap.h>

namespace GC {

HandleImpl::HandleImpl(Cell* cell, SourceLocation location)
    : m_cell(cell)
    , m_location(location)
{
    HeapBlockBase::from_cell(m_cell)->heap().did_create_handle({}, *this);
}

HandleImpl::~HandleImpl()
{
    HeapBlockBase::from_cell(m_cell)->heap().did_destroy_handle({}, *this);
}

}
