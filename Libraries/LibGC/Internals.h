/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Export.h>
#include <LibGC/Forward.h>

namespace GC {

class GC_API HeapBlockBase {
    AK_MAKE_NONMOVABLE(HeapBlockBase);
    AK_MAKE_NONCOPYABLE(HeapBlockBase);

public:
    static size_t block_size;
    static HeapBlockBase* from_cell(Cell const* cell)
    {
        return reinterpret_cast<HeapBlockBase*>(bit_cast<FlatPtr>(cell) & ~(HeapBlockBase::block_size - 1));
    }

    Heap& heap() { return m_heap; }

protected:
    HeapBlockBase(Heap& heap)
        : m_heap(heap)
    {
    }

    Heap& m_heap;
};

}
