/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibTest/TestSuite.h>

#ifndef AK_OS_WINDOWS
#    include <LibThreading/Thread.h>
#    include <unistd.h>
#endif

TEST_CASE(shared_single_producer_circular_buffer_wraparound_single_thread)
{
    auto buffer_or_error = Core::SharedSingleProducerCircularBuffer::create(256);
    EXPECT(!buffer_or_error.is_error());

    auto buffer = buffer_or_error.release_value();

    // Fill 200 bytes
    Array<u8, 200> input;
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<u8>(i);

    EXPECT_EQ(buffer.try_write(input.span()), input.size());

    // Read 150 bytes
    Array<u8, 150> first_read;
    EXPECT_EQ(buffer.try_read(first_read.span()), first_read.size());
    for (size_t i = 0; i < first_read.size(); ++i)
        EXPECT_EQ(first_read[i], static_cast<u8>(i));

    // Write another 200 bytes (forces wrap-around)
    Array<u8, 200> input2;
    for (size_t i = 0; i < input2.size(); ++i)
        input2[i] = static_cast<u8>(200 + i);

    EXPECT_EQ(buffer.try_write(input2.span()), input2.size());

    // Read remaining (50 from first batch + 200 from second)
    Array<u8, 250> second_read;
    EXPECT_EQ(buffer.try_read(second_read.span()), second_read.size());

    for (size_t i = 0; i < 50; ++i)
        EXPECT_EQ(second_read[i], static_cast<u8>(150 + i));

    for (size_t i = 0; i < 200; ++i)
        EXPECT_EQ(second_read[50 + i], static_cast<u8>(200 + i));

    // Now empty
    Array<u8, 1> tmp;
    EXPECT_EQ(buffer.try_read(tmp.span()), 0u);
}

#ifndef AK_OS_WINDOWS
TEST_CASE(shared_single_producer_circular_buffer_two_threads_ordered_u32)
{
    auto buffer_or_error = Core::SharedSingleProducerCircularBuffer::create(1 << 16);
    EXPECT(!buffer_or_error.is_error());

    auto buffer = buffer_or_error.release_value();

    Atomic<bool> done { false };
    Atomic<bool> failed { false };

    constexpr u32 iterations = 50'000;

    auto consumer = MUST(Threading::Thread::try_create([&]() -> intptr_t {
        u32 expected = 0;
        Array<u8, sizeof(u32)> tmp;
        while (expected < iterations) {
            if (buffer.try_read(tmp.span()) != sizeof(u32)) {
                usleep(0);
                continue;
            }
            u32 value = 0;
            __builtin_memcpy(&value, tmp.data(), sizeof(u32));
            if (value != expected) {
                failed.store(true, AK::MemoryOrder::memory_order_release);
                break;
            }
            ++expected;
        }
        done.store(true, AK::MemoryOrder::memory_order_release);
        return 0;
    },
        "SPCB consumer"sv));

    auto producer = MUST(Threading::Thread::try_create([&]() -> intptr_t {
        for (u32 i = 0; i < iterations; ++i) {
            Array<u8, sizeof(u32)> tmp;
            __builtin_memcpy(tmp.data(), &i, sizeof(u32));
            while (buffer.try_write(tmp.span()) != sizeof(u32))
                usleep(0);
            if (failed.load(AK::MemoryOrder::memory_order_acquire))
                break;
        }
        return 0;
    },
        "SPCB producer"sv));

    producer->start();
    consumer->start();

    (void)producer->join();
    (void)consumer->join();

    EXPECT(done.load(AK::MemoryOrder::memory_order_acquire));
    EXPECT(!failed.load(AK::MemoryOrder::memory_order_acquire));
}
#endif
