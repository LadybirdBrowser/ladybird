/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>
#include <LibGC/Cell.h>
#include <LibGC/CellTypeInfo.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

namespace GC {

class GC_API HeapBlock : public HeapBlockBase {
    AK_MAKE_NONCOPYABLE(HeapBlock);
    AK_MAKE_NONMOVABLE(HeapBlock);

public:
    using HeapBlockBase::BLOCK_SIZE;
    static NonnullOwnPtr<HeapBlock> create(Heap&, CellAllocator&);

    size_t cell_size() const { return m_cell_size; }
    size_t cell_count() const { return (HeapBlock::BLOCK_SIZE - sizeof(HeapBlock)) / m_cell_size; }
    bool is_full() const { return !has_lazy_freelist() && is_end_of_freelist(freelist_head()); }

    ALWAYS_INLINE Cell* allocate()
    {
        Cell* allocated_cell = nullptr;
        if (auto* head = freelist_head(); !is_end_of_freelist(head)) {
            VERIFY(is_valid_cell_pointer(head));
            m_freelist = static_cast<FreelistEntry*>(head)->next;
            allocated_cell = head;
        } else if (has_lazy_freelist()) {
            allocated_cell = cell(m_next_lazy_freelist_index++);
        }

        if (allocated_cell) {
            ASAN_UNPOISON_MEMORY_REGION(allocated_cell, m_cell_size);
        }
        return allocated_cell;
    }

    void deallocate(Cell*);

    // Takes every free cell of the block as a list linked through
    // freelist_next_offset, leaving the block full. The cells are dead.
    Cell* take_free_cells();

    // Gives back a list of free cells taken from this block.
    void give_back_free_cells(Cell*);

    // The offset of the link to the next cell in a free cell.
    static constexpr size_t freelist_next_offset();

    // Free cells are in the heap region, where anything may have been
    // corrupted, so a link read from a free cell must never lead out of its
    // block: only the bits of the link under freelist_link_mask count, as
    // the offset of the next cell in the block of the cell holding the link
    // (or of the block holding the head of the list). Offset 0, where the
    // block's header is, ends the list, so a null link ends it as well.
    static constexpr FlatPtr freelist_link_mask = BLOCK_SIZE - 1;
    static Cell* follow_freelist_link(void const* holder, FlatPtr link)
    {
        return bit_cast<Cell*>((bit_cast<FlatPtr>(holder) & ~freelist_link_mask) | (link & freelist_link_mask));
    }
    static Cell* next_free_cell(Cell const* free_cell)
    {
        return follow_freelist_link(free_cell, *bit_cast<FlatPtr const*>(bit_cast<FlatPtr>(free_cell) + freelist_next_offset()));
    }
    static bool is_end_of_freelist(Cell const* cell) { return !(bit_cast<FlatPtr>(cell) & freelist_link_mask); }

    // Whether the incremental sweep has yet to sweep the block, or is
    // sweeping it right now. Cells allocated in such a block during the
    // sweep must be marked so that the sweep keeps them.
    bool is_pending_sweep() const { return m_sweep_list_node.is_in_list(); }
    bool is_being_swept() const { return m_is_being_swept; }
    void set_being_swept(bool being_swept) { m_is_being_swept = being_swept; }

    template<typename Callback>
    void for_each_cell(Callback callback)
    {
        auto end = has_lazy_freelist() ? m_next_lazy_freelist_index : cell_count();
        for (size_t i = 0; i < end; ++i)
            callback(cell(i));
    }

    template<Cell::State state, typename Callback>
    void for_each_cell_in_state(Callback callback)
    {
        for_each_cell([&](auto* cell) {
            if (cell->state() == state)
                callback(cell);
        });
    }

    static HeapBlock* from_cell(Cell const* cell)
    {
        return static_cast<HeapBlock*>(HeapBlockBase::from_cell(cell));
    }

    Cell* cell_from_possible_pointer(FlatPtr pointer)
    {
        if (pointer < reinterpret_cast<FlatPtr>(m_storage))
            return nullptr;
        size_t cell_index = (pointer - reinterpret_cast<FlatPtr>(m_storage)) / m_cell_size;
        auto end = has_lazy_freelist() ? m_next_lazy_freelist_index : cell_count();
        if (cell_index >= end)
            return nullptr;
        return cell(cell_index);
    }

    bool is_valid_cell_pointer(Cell const* cell)
    {
        return cell_from_possible_pointer((FlatPtr)cell);
    }

    IntrusiveListNode<HeapBlock> m_list_node;
    IntrusiveListNode<HeapBlock> m_sweep_list_node;

    CellAllocator& cell_allocator() { return m_cell_allocator; }

private:
    HeapBlock(Heap&, CellAllocator&);

    bool has_lazy_freelist() const { return m_next_lazy_freelist_index < cell_count(); }

    struct FreelistEntry final : public Cell {
        GC_CELL(FreelistEntry, Cell);

        // See follow_freelist_link().
        FlatPtr next { 0 };
    };

    Cell* freelist_head() const { return follow_freelist_link(this, m_freelist); }

    Cell* cell(size_t index)
    {
        return reinterpret_cast<Cell*>(&m_storage[index * cell_size()]);
    }

    CellAllocator& m_cell_allocator;
    u32 m_cell_size { 0 };
    u32 m_next_lazy_freelist_index { 0 };
    bool m_is_being_swept { false };

    // See follow_freelist_link().
    FlatPtr m_freelist { 0 };
    alignas(__BIGGEST_ALIGNMENT__) u8 m_storage[];

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
