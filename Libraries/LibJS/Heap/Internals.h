/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibJS/Forward.h>

namespace JS {

class HeapBase {
    AK_MAKE_NONCOPYABLE(HeapBase);
    AK_MAKE_NONMOVABLE(HeapBase);

public:
    VM& vm() { return m_vm; }

protected:
    HeapBase(VM& vm)
        : m_vm(vm)
    {
    }

    VM& m_vm;
};

class HeapBlockBase {
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
