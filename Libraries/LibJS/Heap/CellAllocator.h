/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullOwnPtr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/BlockAllocator.h>
#include <LibJS/Heap/HeapBlock.h>

#define JS_DECLARE_ALLOCATOR(ClassName) \
    static JS::TypeIsolatingCellAllocator<ClassName> cell_allocator

#define JS_DEFINE_ALLOCATOR(ClassName) \
    JS::TypeIsolatingCellAllocator<ClassName> ClassName::cell_allocator { #ClassName }

namespace JS {

class CellAllocator {
public:
    CellAllocator(size_t cell_size, char const* class_name = nullptr);
    ~CellAllocator() = default;

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
    char const* const m_class_name { nullptr };
    size_t const m_cell_size;

    BlockAllocator m_block_allocator;

    using BlockList = IntrusiveList<&HeapBlock::m_list_node>;
    BlockList m_full_blocks;
    BlockList m_usable_blocks;
    FlatPtr m_min_block_address { explode_byte(0xff) };
    FlatPtr m_max_block_address { 0 };
};

template<typename T>
class TypeIsolatingCellAllocator {
public:
    using CellType = T;

    TypeIsolatingCellAllocator(char const* class_name)
        : allocator(sizeof(T), class_name)
    {
    }

    NeverDestroyed<CellAllocator> allocator;
};

}
