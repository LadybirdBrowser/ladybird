/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/IterationDecision.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CellTypeInfo.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/Ptr.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

// The default allocator, which isolates different Cell types from being allocated in the same blocks.
#define GC_DECLARE_ALLOCATOR(ClassName)    \
    using gc_allocator_marker = ClassName; \
    static GC::TypeIsolatingCellAllocator<ClassName> cell_allocator

#define GC_DEFINE_ALLOCATOR(ClassName) \
    GC::TypeIsolatingCellAllocator<ClassName> ClassName::cell_allocator { #ClassName##sv }

namespace GC::FFI {

struct Allocator;

}

namespace GC {

// Which threads may trace the cells of an allocator while the collector marks. C++ cells are only traced on the thread
// that owns their heap.
enum class TraceThread : u8 {
    OwnerThreadOnly,
    AnyThread,
};

class GC_API CellAllocatorDescriptorBase {
    AK_MAKE_NONCOPYABLE(CellAllocatorDescriptorBase);
    AK_MAKE_NONMOVABLE(CellAllocatorDescriptorBase);

public:
    AK_ALLOC_WITH_KMALLOC;

    Optional<StringView> class_name() const { return m_class_name; }
    CellTypeInfo const& type_info() const { return m_type_info; }
    size_t cell_size() const { return m_type_info.cell_size; }
    void const* foreign_type() const { return m_foreign_type; }
    TraceThread trace_thread() const { return m_trace_thread; }
    void set_trace_thread(TraceThread trace_thread) { m_trace_thread = trace_thread; }

    // Whether collections may move the cells to other blocks of their allocator, see gc_allocator_set_movable(). Only
    // allocators of cells defined outside of C++ are ever movable.
    bool is_movable() const { return m_movable; }
    void set_movable() { m_movable = true; }

    // Where each heap keeps its allocator for this descriptor, see Heap::cell_allocator_for().
    size_t index() const { return m_index; }

    // The blocks of the type of this descriptor, for every heap of the process and for as long as the process lives.
    BlockAllocator& block_allocator() const;

protected:
    CellAllocatorDescriptorBase(CellTypeInfo const& type_info, StringView class_name, void const* foreign_type = nullptr);

    // A descriptor that goes away while the process lives must call this, so that its index does not go to waste. No
    // heap may have an allocator for it any more.
    void release_index();

private:
    CellTypeInfo const& m_type_info;
    Optional<StringView> m_class_name;
    void const* m_foreign_type { nullptr };
    size_t m_index { 0 };
    TraceThread m_trace_thread { TraceThread::OwnerThreadOnly };
    bool m_movable { false };
};

// The cells of one type in one heap. LibGC's Rust core keeps the allocator's blocks, hands out their free cells in runs
// and sweeps them, see allocator.rs. This keeps the run that cells are allocated from inline.
class GC_API CellAllocator {
    AK_MAKE_NONCOPYABLE(CellAllocator);
    AK_MAKE_NONMOVABLE(CellAllocator);

public:
    AK_ALLOC_WITH_KMALLOC;

    CellAllocator(Heap&, CellAllocatorDescriptorBase&);
    ~CellAllocator();

    CellAllocatorDescriptorBase const& descriptor() const { return m_descriptor; }
    Optional<StringView> class_name() const { return m_descriptor.class_name(); }
    size_t cell_size() const { return m_descriptor.cell_size(); }
    CellTypeInfo const& type_info() const { return m_descriptor.type_info(); }
    void const* foreign_type() const { return m_descriptor.foreign_type(); }

    // Cells come from a run of free cells that the allocator took from one
    // of its blocks (the local block) at once, so allocating is usually just
    // bumping a cursor.
    //
    // Invariants of the local free cells:
    // - They are free cells of the local block, whose allocated bits are set
    //   while they are out, see BlockDescriptor.h.
    // - The local block is never pending incremental sweep: they come from
    //   swept or new blocks only, and are handed back to their block whenever
    //   a collection starts. So cells from them never need marking.
    // - The list is always empty, which its head being null says. It is only
    //   there for code that allocates from it itself, which still checks it,
    //   see gc_heap_allocator_local_free_list().
    // - The cells from the cursor up to the end are cell_size apart. There
    //   are none when the cursor is at the end.
    struct LocalFreeCells {
        RawPtr<Cell> free_list;
        FlatPtr cursor { 0 };
        FlatPtr end { 0 };
        FlatPtr cell_size { 0 };
    };

    ALWAYS_INLINE Cell* allocate_cell(Heap& heap)
    {
        if (m_local.cursor < m_local.end) [[likely]] {
            auto* cell = bit_cast<Cell*>(m_local.cursor);
            m_local.cursor += m_local.cell_size;
            ASAN_UNPOISON_MEMORY_REGION(cell, cell_size());
            return cell;
        }
        return allocate_cell_slow(heap);
    }

    // Hands the local free cells back to the local block.
    void give_back_local_free_list(Badge<Heap>);

    // For code that allocates from the local free cells itself, see gc_heap_allocator_local_free_list().
    LocalFreeCells* local_free_cells(Badge<CAPI>) { return &m_local; }

    template<typename Callback>
    IterationDecision for_each_block(Callback callback)
    {
        for (size_t i = 0; i < block_count(); ++i) {
            if (callback(*block_at(i)) == IterationDecision::Break)
                return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    }

    size_t block_count() const;
    bool is_local_block(HeapBlock const&) const;
    bool has_blocks_pending_sweep() const;

    IntrusiveListNode<CellAllocator> m_list_node;
    using List = IntrusiveList<&CellAllocator::m_list_node>;

    IntrusiveListNode<CellAllocator> m_sweep_list_node;
    using SweepList = IntrusiveList<&CellAllocator::m_sweep_list_node>;

    BlockAllocator& block_allocator() { return m_block_allocator; }
    FFI::Allocator* rust_allocator(Badge<Heap>) { return m_allocator; }

private:
    friend class HeapBlock;

    HeapBlock* block_at(size_t index) const;
    Cell* allocate_cell_slow(Heap&);
    static void initialize_block(void* context, void* block);
    static void free_cell(void* context, void* cell, bool destroy);

    CellAllocatorDescriptorBase& m_descriptor;
    LocalFreeCells m_local;
    BlockAllocator& m_block_allocator;
    Heap& m_heap;
    FFI::Allocator* m_allocator { nullptr };
};

template<typename T>
class GC_API TypeIsolatingCellAllocator final : public CellAllocatorDescriptorBase {
public:
    using CellType = T;

    explicit TypeIsolatingCellAllocator(StringView class_name)
        : CellAllocatorDescriptorBase(cell_type_info_for<T>, class_name)
    {
    }
};

// Heap::allocate<T>() takes every cell from T::cell_allocator, and a subclass of T must declare an allocator of its own,
// so a cell that came from T::cell_allocator is exactly a T. This identifies a cell's type without reading its vtable.
inline bool cell_was_allocated_from(Cell const& cell, CellAllocatorDescriptorBase const& descriptor)
{
    return &HeapBlock::from_cell(&cell)->cell_allocator().descriptor() == &descriptor;
}

}
