/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
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
    static constexpr size_t BLOCK_SIZE = 16 * KiB;
    static HeapBlockBase* from_cell(Cell const* cell)
    {
        return reinterpret_cast<HeapBlockBase*>(bit_cast<FlatPtr>(cell) & ~(BLOCK_SIZE - 1));
    }

    Heap& heap() { return m_heap; }
    CellTypeInfo const& type_info() const { return *m_type_info; }

    // What the foreign implementation that created the block's allocator says the type of its cells is.
    void const* foreign_type() const { return m_foreign_type; }

protected:
    HeapBlockBase(Heap& heap, CellTypeInfo const& type_info, void const* foreign_type)
        : m_heap(heap)
        , m_type_info(&type_info)
        , m_foreign_type(foreign_type)
    {
    }

    Heap& m_heap;
    CellTypeInfo const* m_type_info { nullptr };
    void const* m_foreign_type { nullptr };

private:
    friend struct CAPI;
};

}
