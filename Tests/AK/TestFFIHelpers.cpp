/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FFIHelpers.h>
#include <AK/kmalloc.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

TEST_CASE(allocator_preserves_alignment_zeroing_and_contents_across_reallocation)
{
    for (size_t alignment : { 1, 2, 8, 16, 64, 4096 }) {
        auto* pointer = static_cast<u8*>(ladybird_alloc_zeroed(31, alignment));
        VERIFY(pointer);
        EXPECT_EQ(reinterpret_cast<FlatPtr>(pointer) % alignment, 0u);
        for (size_t i = 0; i < 31; ++i)
            EXPECT_EQ(pointer[i], 0);
        __builtin_memset(pointer, 0x5a, 31);
        auto* grown = static_cast<u8*>(ladybird_realloc(pointer, 31, 127, alignment));
        VERIFY(grown);
        EXPECT_EQ(reinterpret_cast<FlatPtr>(grown) % alignment, 0u);
        for (size_t i = 0; i < 31; ++i)
            EXPECT_EQ(grown[i], 0x5a);
        auto* shrunk = static_cast<u8*>(ladybird_realloc(grown, 127, 7, alignment));
        VERIFY(shrunk);
        EXPECT_EQ(reinterpret_cast<FlatPtr>(shrunk) % alignment, 0u);
        for (size_t i = 0; i < 7; ++i)
            EXPECT_EQ(shrunk[i], 0x5a);
        if (alignment <= alignof(max_align_t))
            ak_kfree(shrunk);
        else
            ladybird_dealloc(shrunk, alignment);
    }
}

TEST_CASE(allocator_can_release_worker_allocations_on_the_receiving_thread)
{
    auto thread = Threading::Thread::construct("AllocatorTest"sv, [] {
        auto* pointer = static_cast<u8*>(ladybird_alloc(257, 64));
        VERIFY(pointer);
        __builtin_memset(pointer, 0x7b, 257);
        return reinterpret_cast<intptr_t>(pointer);
    });
    thread->start();
    auto* pointer = MUST(thread->join<u8*>());
    for (size_t i = 0; i < 257; ++i)
        EXPECT_EQ(pointer[i], 0x7b);
    ladybird_dealloc(pointer, 64);
}
