/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/CellImpl.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Heap/Heap.h>

namespace JS {

HandleImpl::HandleImpl(CellImpl* cell, SourceLocation location)
    : m_cell(cell)
    , m_location(location)
{
    m_cell->heap().did_create_handle({}, *this);
}

HandleImpl::~HandleImpl()
{
    m_cell->heap().did_destroy_handle({}, *this);
}

}
