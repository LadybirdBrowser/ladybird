/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>
#include <LibGC/Cell.h>
#include <LibGC/CellTypeInfo.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>

namespace GC {

// The header of a block of cells. LibGC's Rust core creates blocks and keeps track of which of their cells are
// allocated, in the block's descriptor, see BlockDescriptor.h. The header is only what C++ reads from a cell's block.
class GC_API HeapBlock : public HeapBlockBase {
    AK_MAKE_NONCOPYABLE(HeapBlock);
    AK_MAKE_NONMOVABLE(HeapBlock);

public:
    using HeapBlockBase::BLOCK_SIZE;

    // Fills in the header of a block that an allocator just took.
    static void initialize(void* block, Heap&, CellAllocator&);

    size_t cell_size() const { return m_cell_size; }
    size_t cell_count() const { return m_cell_count; }

    // The offset of the link to the next cell in a free cell, and the bits of a link that count, for code that pops
    // free lists itself, see gc_heap_allocator_local_free_list(). The allocator hands out runs of cells only, so the
    // lists it hands out are empty.
    static constexpr size_t freelist_next_offset();
    static constexpr FlatPtr freelist_link_mask = BLOCK_SIZE - 1;

    // How far into a block its first cell is.
    static size_t cells_offset();

    // Whether the incremental sweep has yet to sweep the block, or is
    // sweeping it right now. Cells allocated in such a block during the
    // sweep must be marked so that the sweep keeps them.
    bool is_pending_sweep() const;
    bool is_being_swept() const;

    // How many cells of the block are allocated. The cells of a run that an allocator took from the block to hand out
    // count as allocated too, see CellAllocator::LocalFreeCells.
    u32 allocated_cell_count() const;

    static HeapBlock* from_cell(Cell const* cell)
    {
        return static_cast<HeapBlock*>(HeapBlockBase::from_cell(cell));
    }

    CellAllocator& cell_allocator() { return m_cell_allocator; }

private:
    HeapBlock(Heap&, CellAllocator&);

    struct FreelistEntry final : public Cell {
        GC_CELL(FreelistEntry, Cell);

        FlatPtr next { 0 };
    };

    CellAllocator& m_cell_allocator;
    u32 m_cell_size { 0 };
    // NB: Computed once, since dividing by the cell size is slow.
    u32 m_cell_count { 0 };
    // NB: Pads the header to the size that sets how many cells of each size a block holds, see cells_offset().
    [[maybe_unused]] u8 m_reserved[72];
    [[maybe_unused]] alignas(max_cell_alignment) u8 m_storage[];

public:
    static constexpr size_t min_possible_cell_size = sizeof(FreelistEntry);
};

constexpr size_t HeapBlock::freelist_next_offset()
{
    return __builtin_offsetof(FreelistEntry, next);
}

}

template<>
inline constexpr bool AllocatedWithCustomAllocator<GC::HeapBlock> = true;
