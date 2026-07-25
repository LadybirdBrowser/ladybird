/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/TearableAtomic.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

struct MultiWordValue {
    u64 first;
    u64 second;
    u64 third;
};

struct OddSizedValue {
    u64 word;
    u16 half;
    u8 tail;
};

TEST_CASE(values_round_trip)
{
    TearableAtomic<MultiWordValue> multi_word;
    multi_word.store_relaxed({ 1, 2, 3 });
    auto multi_word_value = multi_word.load_relaxed();
    EXPECT_EQ(multi_word_value.first, 1u);
    EXPECT_EQ(multi_word_value.second, 2u);
    EXPECT_EQ(multi_word_value.third, 3u);

    TearableAtomic<OddSizedValue> odd_sized;
    odd_sized.store_relaxed({ 0x1122334455667788, 0x99aa, 0xbb });
    auto odd_sized_value = odd_sized.load_relaxed();
    EXPECT_EQ(odd_sized_value.word, 0x1122334455667788u);
    EXPECT_EQ(odd_sized_value.half, 0x99aa);
    EXPECT_EQ(odd_sized_value.tail, 0xbb);

    TearableAtomic<u8> single_byte;
    single_byte.store_relaxed(0x42);
    EXPECT_EQ(single_byte.load_relaxed(), 0x42);
}

TEST_CASE(default_value_is_zeroed)
{
    TearableAtomic<MultiWordValue> value;
    auto loaded = value.load_relaxed();
    EXPECT_EQ(loaded.first, 0u);
    EXPECT_EQ(loaded.second, 0u);
    EXPECT_EQ(loaded.third, 0u);
}

// Concurrent loads may tear between words, but every byte must come from some stored value; a byte
// with any other value means an access was widened, torn within a word, or invented.
TEST_CASE(concurrent_loads_observe_only_stored_bytes)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA TearableAtomic<OddSizedValue> value;
    value.store_relaxed({ 0, 0, 0 });
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> stop { false };

    auto writer = Threading::Thread::construct("TearableAtomicWriter"sv, [&value, &stop]() {
        bool ones = false;
        while (!stop.load(AK::MemoryOrder::memory_order_relaxed)) {
            ones = !ones;
            u8 fill = ones ? 0xff : 0x00;
            u64 word_fill = ones ? 0xffffffffffffffffu : 0u;
            u16 half_fill = ones ? 0xffffu : 0u;
            value.store_relaxed({ word_fill, half_fill, fill });
        }
        return 0;
    });
    writer->start();

    for (size_t iteration = 0; iteration < 500'000; iteration++) {
        auto snapshot = value.load_relaxed();
        auto const* bytes = reinterpret_cast<u8 const*>(&snapshot);
        for (size_t i = 0; i <= offsetof(OddSizedValue, tail); i++)
            EXPECT(bytes[i] == 0x00 || bytes[i] == 0xff);
    }

    stop.store(true, AK::MemoryOrder::memory_order_relaxed);
    (void)writer->join();
}
