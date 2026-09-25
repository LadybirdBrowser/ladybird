/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CellTypeInfo.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapBlock.h>

// The default allocator, which isolates different Cell types from being allocated in the same blocks.
#define GC_DECLARE_ALLOCATOR(ClassName)    \
    using gc_allocator_marker = ClassName; \
    static GC::TypeIsolatingCellAllocator<ClassName> cell_allocator

#define GC_DEFINE_ALLOCATOR(ClassName) \
    GC::TypeIsolatingCellAllocator<ClassName> ClassName::cell_allocator { #ClassName##sv }

namespace GC {

class GC_API CellAllocatorDescriptorBase {
    AK_MAKE_NONCOPYABLE(CellAllocatorDescriptorBase);
    AK_MAKE_NONMOVABLE(CellAllocatorDescriptorBase);

public:
    Optional<StringView> class_name() const { return m_class_name; }
    CellTypeInfo const& type_info() const { return m_type_info; }
    size_t cell_size() const { return m_type_info.cell_size; }

    CellAllocator& for_heap(Heap&);

    void forget_heap(Badge<Heap>, Heap& heap)
    {
        if (m_last_heap == &heap) {
            m_last_heap = nullptr;
            m_last_allocator = nullptr;
        }
    }

protected:
    CellAllocatorDescriptorBase(CellTypeInfo const& type_info, StringView class_name)
        : m_type_info(type_info)
        , m_class_name(class_name)
    {
    }

private:
    CellTypeInfo const& m_type_info;
    Optional<StringView> m_class_name;

    Heap* m_last_heap { nullptr };
    CellAllocator* m_last_allocator { nullptr };
};

class GC_API CellAllocator {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit CellAllocator(CellAllocatorDescriptorBase&);
    ~CellAllocator();

    static BlockAllocator& shared_block_allocator();

    Optional<StringView> class_name() const { return m_descriptor.class_name(); }
    size_t cell_size() const { return m_descriptor.cell_size(); }
    CellTypeInfo const& type_info() const { return m_descriptor.type_info(); }

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

private:
    friend class Heap;

    CellAllocatorDescriptorBase& m_descriptor;

    BlockAllocator& m_block_allocator;

    using BlockList = IntrusiveList<&HeapBlock::m_list_node>;
    using SweepBlockList = IntrusiveList<&HeapBlock::m_sweep_list_node>;
    BlockList m_full_blocks;
    BlockList m_usable_blocks;
    SweepBlockList m_blocks_pending_sweep;
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

}
