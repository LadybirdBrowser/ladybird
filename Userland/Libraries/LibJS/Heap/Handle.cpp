/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

HandleImpl::HandleImpl(Cell* cell, SourceLocation location)
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
