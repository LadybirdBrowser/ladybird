/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapBlock.h>

// The default allocator, which isolates different Cell types from being allocated in the same blocks.
#define GC_DECLARE_ALLOCATOR(ClassName)    \
    using gc_allocator_marker = ClassName; \
    static GC::TypeIsolatingCellAllocator<ClassName> cell_allocator

#define GC_DEFINE_ALLOCATOR(ClassName) \
    GC::TypeIsolatingCellAllocator<ClassName> ClassName::cell_allocator { #ClassName##sv, ClassName::OVERRIDES_MUST_SURVIVE_GARBAGE_COLLECTION, ClassName::OVERRIDES_FINALIZE }

namespace GC {

class GC_API CellAllocatorDescriptorBase {
    AK_MAKE_NONCOPYABLE(CellAllocatorDescriptorBase);
    AK_MAKE_NONMOVABLE(CellAllocatorDescriptorBase);

public:
    Optional<StringView> class_name() const { return m_class_name; }
    size_t cell_size() const { return m_cell_size; }
    bool overrides_must_survive_garbage_collection() const { return m_overrides_must_survive_garbage_collection; }
    bool overrides_finalize() const { return m_overrides_finalize; }

    CellAllocator& for_heap(Heap&);

    void forget_heap(Badge<Heap>, Heap& heap)
    {
        if (m_last_heap == &heap) {
            m_last_heap = nullptr;
            m_last_allocator = nullptr;
        }
    }

protected:
    CellAllocatorDescriptorBase(size_t cell_size, StringView class_name, bool overrides_must_survive_garbage_collection, bool overrides_finalize)
        : m_class_name(class_name)
        , m_cell_size(cell_size)
        , m_overrides_must_survive_garbage_collection(overrides_must_survive_garbage_collection)
        , m_overrides_finalize(overrides_finalize)
    {
    }

private:
    Optional<StringView> m_class_name;
    size_t m_cell_size { 0 };
    bool m_overrides_must_survive_garbage_collection { false };
    bool m_overrides_finalize { false };

    Heap* m_last_heap { nullptr };
    CellAllocator* m_last_allocator { nullptr };
};

class GC_API CellAllocator {
public:
    CellAllocator(size_t cell_size, Optional<StringView> = {}, bool overrides_must_survive_garbage_collection = false, bool overrides_finalize = false);
    ~CellAllocator();

    static BlockAllocator& shared_block_allocator();

    Optional<StringView> class_name() const { return m_class_name; }
    size_t cell_size() const { return m_cell_size; }

    Cell* allocate_cell(Heap&);

    template<typename Callback>
    IterationDecision for_each_block(Callback callback)
    {
        for (auto& block : m_full_blocks) {
            if (callback(block) == IterationDecision::Break)
                return IterationDecision::Break;
        }
        for (auto& block : m_usable_blocks) {
            if (callback(block) == IterationDecision::Break)
                return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    }

    void block_did_become_empty(Badge<Heap>, HeapBlock&, DeferDecommit = DeferDecommit::Yes);
    void block_did_become_usable(Badge<Heap>, HeapBlock&);

    bool has_blocks_pending_sweep() const { return !m_blocks_pending_sweep.is_empty(); }

    IntrusiveListNode<CellAllocator> m_list_node;
    using List = IntrusiveList<&CellAllocator::m_list_node>;

    IntrusiveListNode<CellAllocator> m_sweep_list_node;
    using SweepList = IntrusiveList<&CellAllocator::m_sweep_list_node>;

    BlockAllocator& block_allocator() { return m_block_allocator; }
    FlatPtr min_block_address() const { return m_min_block_address; }
    FlatPtr max_block_address() const { return m_max_block_address; }

private:
    friend class Heap;

    Optional<StringView> m_class_name;
    size_t const m_cell_size;

    BlockAllocator& m_block_allocator;

    using BlockList = IntrusiveList<&HeapBlock::m_list_node>;
    using SweepBlockList = IntrusiveList<&HeapBlock::m_sweep_list_node>;
    BlockList m_full_blocks;
    BlockList m_usable_blocks;
    SweepBlockList m_blocks_pending_sweep;
    FlatPtr m_min_block_address { explode_byte(0xff) };
    FlatPtr m_max_block_address { 0 };
    bool m_overrides_must_survive_garbage_collection { false };
    bool m_overrides_finalize { false };
};

template<typename T>
class GC_API TypeIsolatingCellAllocator final : public CellAllocatorDescriptorBase {
public:
    using CellType = T;

    TypeIsolatingCellAllocator(StringView class_name, bool overrides_must_survive_garbage_collection, bool overrides_finalize)
        : CellAllocatorDescriptorBase(sizeof(T), class_name, overrides_must_survive_garbage_collection, overrides_finalize)
    {
    }
};

}
