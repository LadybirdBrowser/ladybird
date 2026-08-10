/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/BlockAllocator.h>
#include <LibGC/HeapBlock.h>
#include <LibTest/TestCase.h>

TEST_CASE(all_blocks_share_one_reserved_region)
{
    GC::BlockAllocator first_allocator;
    GC::BlockAllocator second_allocator;

    auto* first_block = first_allocator.allocate_block("first");
    auto* second_block = second_allocator.allocate_block("second");

    auto region_start = GC::BlockAllocator::heap_region_start();
    auto region_end = GC::BlockAllocator::heap_region_end();
    EXPECT(region_start < region_end);

    auto first_address = reinterpret_cast<FlatPtr>(first_block);
    auto second_address = reinterpret_cast<FlatPtr>(second_block);
    EXPECT(first_address >= region_start);
    EXPECT(first_address + GC::HeapBlock::BLOCK_SIZE <= region_end);
    EXPECT(second_address >= region_start);
    EXPECT(second_address + GC::HeapBlock::BLOCK_SIZE <= region_end);

    first_allocator.deallocate_block(first_block, GC::DeferDecommit::No);
    second_allocator.deallocate_block(second_block, GC::DeferDecommit::No);
}
