/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/IntrusiveList.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibGC/Cell.h>
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
    static NonnullOwnPtr<HeapBlock> create_with_cell_size(Heap&, CellAllocator&, size_t cell_size, StringView class_name, bool overrides_must_survive_garbage_collection, bool overrides_finalize);

    size_t cell_size() const { return m_cell_size; }
    size_t cell_count() const { return (HeapBlock::BLOCK_SIZE - sizeof(HeapBlock)) / m_cell_size; }
    bool is_full() const { return !has_lazy_freelist() && !m_freelist; }

    ALWAYS_INLINE Cell* allocate()
    {
        Cell* allocated_cell = nullptr;
        if (m_freelist) {
            VERIFY(is_valid_cell_pointer(m_freelist));
            allocated_cell = exchange(m_freelist, m_freelist->next);
        } else if (has_lazy_freelist()) {
            allocated_cell = cell(m_next_lazy_freelist_index++);
        }

        if (allocated_cell) {
            ASAN_UNPOISON_MEMORY_REGION(allocated_cell, m_cell_size);
        }
        return allocated_cell;
    }

    void deallocate(Cell*);

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

    bool overrides_must_survive_garbage_collection() const { return m_overrides_must_survive_garbage_collection; }
    bool overrides_finalize() const { return m_overrides_finalize; }

private:
    HeapBlock(Heap&, CellAllocator&, size_t cell_size, bool overrides_must_survive_garbage_collection, bool overrides_finalize);

    bool has_lazy_freelist() const { return m_next_lazy_freelist_index < cell_count(); }

    struct FreelistEntry final : public Cell {
        GC_CELL(FreelistEntry, Cell);

        RawPtr<FreelistEntry> next;
    };

    Cell* cell(size_t index)
    {
        return reinterpret_cast<Cell*>(&m_storage[index * cell_size()]);
    }

    CellAllocator& m_cell_allocator;
    u32 m_cell_size { 0 };
    u32 m_next_lazy_freelist_index { 0 };

    bool m_overrides_must_survive_garbage_collection { false };
    bool m_overrides_finalize { false };

    Ptr<FreelistEntry> m_freelist;

public:
    static constexpr size_t min_possible_cell_size = sizeof(FreelistEntry);

    // Upper bound on cells per block (ignoring bitmap overhead in sizeof(HeapBlock)).
    // The actual cell count is always <= this, so the bitmap is always large enough.
    static constexpr size_t max_cells_per_block = BLOCK_SIZE / min_possible_cell_size;
    static constexpr size_t mark_bitmap_word_count = (max_cells_per_block + 63) / 64;

    ALWAYS_INLINE size_t cell_index(Cell const* cell) const
    {
        return (reinterpret_cast<FlatPtr>(cell) - reinterpret_cast<FlatPtr>(m_storage)) / m_cell_size;
    }

    ALWAYS_INLINE bool is_marked(size_t index) const
    {
        return m_mark_bitmap[index / 64].load(AK::MemoryOrder::memory_order_relaxed) & (1ULL << (index % 64));
    }

    ALWAYS_INLINE void set_marked(size_t index)
    {
        m_mark_bitmap[index / 64].fetch_or(1ULL << (index % 64), AK::MemoryOrder::memory_order_relaxed);
    }

    ALWAYS_INLINE void clear_marked(size_t index)
    {
        m_mark_bitmap[index / 64].fetch_and(~(1ULL << (index % 64)), AK::MemoryOrder::memory_order_relaxed);
    }

    ALWAYS_INLINE void clear_all_marks()
    {
        for (auto& word : m_mark_bitmap)
            word.store(0, AK::MemoryOrder::memory_order_relaxed);
    }

private:
    Atomic<u64> m_mark_bitmap[mark_bitmap_word_count] {};
    alignas(__BIGGEST_ALIGNMENT__) u8 m_storage[];
};

}
