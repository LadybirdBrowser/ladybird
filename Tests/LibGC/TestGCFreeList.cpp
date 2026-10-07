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

}

TEST_CASE(a_corrupted_free_cell_link_leads_to_a_cell_of_the_same_block)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);

    // NB: The first cell comes from a new block, whose free cells the allocator takes in address order.
    auto first = GC::make_root(heap.allocate<FreeListTestCell>());
    auto cell_size = GC::HeapBlock::from_cell(first.ptr())->cell_size();
    auto address_of = [&](size_t index) { return bit_cast<FlatPtr>(first.ptr()) + index * cell_size; };

    // Overwrite the link of the next free cell with an address outside the heap region that has the offset of the
    // fourth cell in its block.
    auto link_mask = GC::HeapBlock::freelist_link_mask;
    auto forged_link = (0x7fff'0000'0000'0000ull & ~link_mask) | (address_of(3) & link_mask);
    *bit_cast<FlatPtr*>(address_of(1) + GC::HeapBlock::freelist_next_offset()) = forged_link;

    auto second = GC::make_root(heap.allocate<FreeListTestCell>());
    EXPECT_EQ(bit_cast<FlatPtr>(second.ptr()), address_of(1));
    auto third = GC::make_root(heap.allocate<FreeListTestCell>());
    EXPECT_EQ(bit_cast<FlatPtr>(third.ptr()), address_of(3));
}
