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

// The size-based allocator, which isolates different Cell types based on their size instead of their concrete type.
// This should only be used if it's not possible or undesirable to use a type-isolated cell allocator.
// Different Cell types can use the same blocks if they happen to have the same size, which allows type confusion
// to occur if a Cell is used after it's freed.
#define GC_DECLARE_SIZE_BASED_ALLOCATOR(ClassName) \
    using gc_allocator_marker = ClassName

namespace GC {

class GC_API CellAllocator {
public:
    CellAllocator(size_t cell_size, Optional<StringView> = {}, bool overrides_must_survive_garbage_collection = false, bool overrides_finalize = false);
    ~CellAllocator() = default;

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

    void block_did_become_empty(Badge<Heap>, HeapBlock&);
    void block_did_become_usable(Badge<Heap>, HeapBlock&);

    IntrusiveListNode<CellAllocator> m_list_node;
    using List = IntrusiveList<&CellAllocator::m_list_node>;

    BlockAllocator& block_allocator() { return m_block_allocator; }
    FlatPtr min_block_address() const { return m_min_block_address; }
    FlatPtr max_block_address() const { return m_max_block_address; }

private:
    Optional<StringView> m_class_name;
    size_t const m_cell_size;

    BlockAllocator m_block_allocator;

    using BlockList = IntrusiveList<&HeapBlock::m_list_node>;
    BlockList m_full_blocks;
    BlockList m_usable_blocks;
    FlatPtr m_min_block_address { explode_byte(0xff) };
    FlatPtr m_max_block_address { 0 };
    bool m_overrides_must_survive_garbage_collection { false };
    bool m_overrides_finalize { false };
};

template<typename T>
class GC_API TypeIsolatingCellAllocator {
public:
    using CellType = T;

    TypeIsolatingCellAllocator(StringView class_name, bool overrides_must_survive_garbage_collection, bool overrides_finalize)
        : allocator(sizeof(T), class_name, overrides_must_survive_garbage_collection, overrides_finalize)
    {
    }

    NeverDestroyed<CellAllocator> allocator;
};

}
