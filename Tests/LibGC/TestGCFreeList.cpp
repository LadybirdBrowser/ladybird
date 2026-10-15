/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>

namespace {

class FreeListTestCell final : public GC::Cell {
    GC_CELL(FreeListTestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(FreeListTestCell);

private:
    FreeListTestCell() = default;

    [[maybe_unused]] FlatPtr m_room_for_a_freelist_entry[2] {};
};

GC_DEFINE_ALLOCATOR(FreeListTestCell);

NEVER_INLINE void allocate_unreachable_cells(GC::Heap& heap, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        (void)heap.allocate<FreeListTestCell>();
}

}

TEST_CASE(a_corrupted_free_cell_link_leads_to_a_cell_of_the_same_block)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    heap.set_incremental_sweep_enabled(false);

    // NB: The cells of a new block come in address order.
    auto first = GC::make_root(heap.allocate<FreeListTestCell>());
    auto cell_size = GC::HeapBlock::from_cell(first.ptr())->cell_size();
    auto address_of = [&](size_t index) { return bit_cast<FlatPtr>(first.ptr()) + index * cell_size; };
    allocate_unreachable_cells(heap, 4);
    GC::scrub_stack_for_testing();

    // The sweep frees the four unreachable cells in address order, so the free list of the block leads from the last
    // of them to the first.
    heap.collect_garbage();

    // Overwrite the link of the cell at the head of the list with an address outside the heap region that has the
    // offset of the second cell after it in its block.
    auto link_mask = GC::HeapBlock::freelist_link_mask;
    auto forged_link = (0x7fff'0000'0000'0000ull & ~link_mask) | (address_of(2) & link_mask);
    *bit_cast<FlatPtr*>(address_of(4) + GC::HeapBlock::freelist_next_offset()) = forged_link;

    auto second = GC::make_root(heap.allocate<FreeListTestCell>());
    EXPECT_EQ(bit_cast<FlatPtr>(second.ptr()), address_of(4));
    auto third = GC::make_root(heap.allocate<FreeListTestCell>());
    EXPECT_EQ(bit_cast<FlatPtr>(third.ptr()), address_of(2));
}
