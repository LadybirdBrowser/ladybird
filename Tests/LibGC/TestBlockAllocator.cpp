/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/System.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/HeapRegion.h>
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
    EXPECT_EQ(region_start, js_heap_region_base);
    EXPECT_EQ(region_start % GC::HEAP_REGION_SIZE, 0u);

    auto first_address = reinterpret_cast<FlatPtr>(first_block);
    auto second_address = reinterpret_cast<FlatPtr>(second_block);
    EXPECT(first_address >= region_start);
    EXPECT(first_address + GC::HeapBlock::BLOCK_SIZE <= region_end);
    EXPECT(second_address >= region_start);
    EXPECT(second_address + GC::HeapBlock::BLOCK_SIZE <= region_end);

    first_allocator.deallocate_block(first_block, GC::DeferDecommit::No);
    second_allocator.deallocate_block(second_block, GC::DeferDecommit::No);
}

TEST_CASE(decommitted_block_can_be_reused)
{
    GC::BlockAllocator allocator;

    auto* block = static_cast<u8*>(allocator.allocate_block("decommit"));
    for (size_t i = 0; i < GC::HeapBlock::BLOCK_SIZE; ++i)
        block[i] = 0xab;
    auto block_count_before_free = allocator.block_count();

    allocator.deallocate_block(block, GC::DeferDecommit::Yes);
    GC::BlockAllocator::wake_decommit_worker_async();

    // The decommit worker returns the block to the reusable pool once it has released the block's pages.
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
    while (allocator.block_count() == block_count_before_free && MonotonicTime::now() < deadline)
        (void)Core::System::sleep_ms(10);
    EXPECT_EQ(allocator.block_count(), block_count_before_free + 1);

    auto* reused_block = static_cast<u8*>(allocator.allocate_block("decommit"));
    EXPECT_EQ(reused_block, block);

    for (size_t i = 0; i < GC::HeapBlock::BLOCK_SIZE; ++i)
        reused_block[i] = static_cast<u8>(i);
    bool contents_match = true;
    for (size_t i = 0; i < GC::HeapBlock::BLOCK_SIZE; ++i) {
        if (reused_block[i] != static_cast<u8>(i))
            contents_match = false;
    }
    EXPECT(contents_match);

    allocator.deallocate_block(reused_block, GC::DeferDecommit::No);
}
