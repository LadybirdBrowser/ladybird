/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::Layout {

NodeArena::NodeArena()
    : m_handle(RustFFI::layout_arena_create())
{
    VERIFY(m_handle);
}

NodeArena::~NodeArena()
{
    RustFFI::layout_arena_destroy(m_handle);
}

RustFFI::NodeAllocation NodeArena::allocate()
{
    auto allocation = RustFFI::layout_arena_allocate(m_handle);
    VERIFY(allocation.data);
    return allocation;
}

void NodeArena::free(RustFFI::NodeSlotId slot, u32 generation)
{
    RustFFI::layout_arena_free(m_handle, slot, generation);
}

}
