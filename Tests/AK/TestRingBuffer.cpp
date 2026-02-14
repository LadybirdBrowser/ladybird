/*
 * Copyright (c) 2026, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/RingBuffer.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

TEST_CASE(mpsc_basic_push_pop)
{
    MPSCRingBuffer<int, 4> buffer;
    int value = 0;

    EXPECT(buffer.try_push(1));
    EXPECT(buffer.try_push(2));
    EXPECT(buffer.try_push(3));

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 1);

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 2);

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 3);

    EXPECT(!buffer.try_pop(value));
}

TEST_CASE(mpsc_buffer_full)
{
    MPSCRingBuffer<int, 4> buffer;
    EXPECT(buffer.try_push(1));
    EXPECT(buffer.try_push(2));
    EXPECT(buffer.try_push(3));
    EXPECT(buffer.try_push(4));

    // Buffer is size 4, so it should be full now
    EXPECT(!buffer.try_push(5));

    int value;
    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 1);

    // Now we should be able to push again
    EXPECT(buffer.try_push(5));
}

TEST_CASE(mpsc_buffer_empty)
{
    MPSCRingBuffer<int, 4> buffer;
    int value;
    EXPECT(!buffer.try_pop(value));
}

TEST_CASE(mpsc_wrap_around_logic)
{
    // Test that the sequence logic handles wrapping correctly
    // Size 2 implies mask is 1. Indices will wrap quickly in the buffer,
    // but the sequence numbers will grow monotonously.
    MPSCRingBuffer<int, 2> buffer;
    int value;

    // Generation 0
    EXPECT(buffer.try_push(10));
    EXPECT(buffer.try_push(11));
    EXPECT(!buffer.try_push(12)); // Full

    EXPECT(buffer.try_pop(value)); // Pops 10
    EXPECT_EQ(value, 10);

    // Generation 1 (for slot 0)
    EXPECT(buffer.try_push(12));  // Pushes to slot 0 (index 2)
    EXPECT(!buffer.try_push(13)); // Full (slot 1 is still occupied by 11)

    EXPECT(buffer.try_pop(value)); // Pops 11
    EXPECT_EQ(value, 11);

    EXPECT(buffer.try_push(13)); // Pushes to slot 1 (index 3)

    EXPECT(buffer.try_pop(value)); // Pops 12
    EXPECT_EQ(value, 12);

    EXPECT(buffer.try_pop(value)); // Pops 13
    EXPECT_EQ(value, 13);
}

TEST_CASE(mpsc_complex_object)
{
    struct Obj {
        int x;
        int y;
        bool operator==(Obj const& other) const { return x == other.x && y == other.y; }
    };

    MPSCRingBuffer<Obj, 4> buffer;
    EXPECT(buffer.try_push({ 1, 2 }));
    EXPECT(buffer.try_push({ 3, 4 }));

    Obj val;
    EXPECT(buffer.try_pop(val));
    EXPECT_EQ(val.x, 1);
    EXPECT_EQ(val.y, 2);

    EXPECT(buffer.try_pop(val));
    EXPECT_EQ(val.x, 3);
    EXPECT_EQ(val.y, 4);
}

TEST_CASE(mpsc_threaded)
{
    static constexpr size_t NUM_PRODUCERS = 4;
    static constexpr size_t ITEMS_PER_PRODUCER = 10000;
    static constexpr size_t BUFFER_SIZE = 64; // Small buffer to force contention

    using RingBufferType = MPSCRingBuffer<int, BUFFER_SIZE>;
    auto buffer = make<RingBufferType>();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> producer_done_count { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> total_consumed { 0 };

    Vector<NonnullRefPtr<Threading::Thread>> producers;

    for (size_t i = 0; i < NUM_PRODUCERS; ++i) {
        auto thread = Threading::Thread::construct(ByteString::formatted("Producer_{}", i), [buffer = buffer.ptr(), id = i, &producer_done_count] {
            for (size_t k = 0; k < ITEMS_PER_PRODUCER; ++k) {
                while (!buffer->try_push(static_cast<int>((id * ITEMS_PER_PRODUCER) + k))) {
                    AK::atomic_pause();
                }
            }
            producer_done_count.fetch_add(1);
            return 0;
        });
        producers.append(thread);
    }

    auto consumer = Threading::Thread::construct("Consumer"sv, [buffer = buffer.ptr(), &total_consumed, &producer_done_count] {
        size_t consumed = 0;
        // Continue consuming until all producers are done AND we have consumed everything
        while (producer_done_count.load() < NUM_PRODUCERS || consumed < (NUM_PRODUCERS * ITEMS_PER_PRODUCER)) {
            int value;
            if (buffer->try_pop(value)) {
                consumed++;
            } else {
                AK::atomic_pause();
            }
        }
        total_consumed.store(consumed);
        return 0;
    });

    consumer->start();
    for (auto& t : producers)
        t->start();

    for (auto& t : producers)
        (void)t->join();
    (void)consumer->join();

    EXPECT_EQ(total_consumed.load(), NUM_PRODUCERS * ITEMS_PER_PRODUCER);
}

BENCHMARK_CASE(mpsc_throughput_batch)
{
    static constexpr size_t NUM_BATCHES = 10'000;
    static constexpr size_t BATCH_SIZE = 128;
    MPSCRingBuffer<int, 256> buffer;

    for (size_t i = 0; i < NUM_BATCHES; ++i) {
        for (size_t j = 0; j < BATCH_SIZE; ++j) {
            while (!buffer.try_push(1))
                ;
        }
        for (size_t j = 0; j < BATCH_SIZE; ++j) {
            int val;
            while (!buffer.try_pop(val))
                ;
        }
    }
}

BENCHMARK_CASE(mpsc_throughput_threaded)
{
    static constexpr size_t NUM_PRODUCERS = 4;
    static constexpr size_t ITEMS_PER_PRODUCER = 1'000'000;
    static constexpr size_t BUFFER_SIZE = 1024;

    using RingBufferType = MPSCRingBuffer<int, BUFFER_SIZE>;
    auto buffer = make<RingBufferType>();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> producer_done_count { 0 };

    Vector<NonnullRefPtr<Threading::Thread>> producers;

    for (size_t i = 0; i < NUM_PRODUCERS; ++i) {
        auto thread = Threading::Thread::construct(ByteString::formatted("Producer_{}", i), [buffer = buffer.ptr(), &producer_done_count] {
            for (size_t k = 0; k < ITEMS_PER_PRODUCER; ++k) {
                while (!buffer->try_push(1)) {
                    AK::atomic_pause();
                }
            }
            producer_done_count.fetch_add(1);
            return 0;
        });
        producers.append(thread);
    }

    auto consumer = Threading::Thread::construct("Consumer"sv, [buffer = buffer.ptr(), &producer_done_count] {
        size_t consumed = 0;
        while (producer_done_count.load() < NUM_PRODUCERS || consumed < (NUM_PRODUCERS * ITEMS_PER_PRODUCER)) {
            int value;
            if (buffer->try_pop(value)) {
                consumed++;
            } else {
                AK::atomic_pause();
            }
        }
        return 0;
    });

    consumer->start();
    for (auto& t : producers)
        t->start();

    for (auto& t : producers)
        (void)t->join();
    (void)consumer->join();
}

TEST_CASE(spsc_basic_push_pop)
{
    SPSCRingBuffer<int, 4> buffer;
    int value = 0;

    EXPECT(buffer.try_push(1));
    EXPECT(buffer.try_push(2));
    EXPECT(buffer.try_push(3));

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 1);

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 2);

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 3);

    EXPECT(!buffer.try_pop(value));
}

TEST_CASE(spsc_buffer_full)
{
    SPSCRingBuffer<int, 4> buffer;
    EXPECT(buffer.try_push(1));
    EXPECT(buffer.try_push(2));
    EXPECT(buffer.try_push(3));
    EXPECT(buffer.try_push(4));

    EXPECT(!buffer.try_push(5));

    int value;
    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 1);

    EXPECT(buffer.try_push(5));
}

TEST_CASE(spsc_buffer_empty)
{
    SPSCRingBuffer<int, 4> buffer;
    int value;
    EXPECT(!buffer.try_pop(value));
}

TEST_CASE(spsc_wrap_around_logic)
{
    SPSCRingBuffer<int, 2> buffer;
    int value;

    EXPECT(buffer.try_push(10));
    EXPECT(buffer.try_push(11));
    EXPECT(!buffer.try_push(12));

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 10);

    EXPECT(buffer.try_push(12));
    EXPECT(!buffer.try_push(13));

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 11);

    EXPECT(buffer.try_push(13));

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 12);

    EXPECT(buffer.try_pop(value));
    EXPECT_EQ(value, 13);
}

TEST_CASE(spsc_complex_object)
{
    struct Obj {
        int x;
        int y;
        bool operator==(Obj const& other) const { return x == other.x && y == other.y; }
    };

    SPSCRingBuffer<Obj, 4> buffer;
    EXPECT(buffer.try_push({ 1, 2 }));
    EXPECT(buffer.try_push({ 3, 4 }));

    Obj val;
    EXPECT(buffer.try_pop(val));
    EXPECT_EQ(val.x, 1);
    EXPECT_EQ(val.y, 2);

    EXPECT(buffer.try_pop(val));
    EXPECT_EQ(val.x, 3);
    EXPECT_EQ(val.y, 4);
}

TEST_CASE(spsc_convertible_push)
{
    SPSCRingBuffer<ByteString, 4> buffer;
    EXPECT(buffer.try_push("foo"));
    ByteString val;
    EXPECT(buffer.try_pop(val));
    EXPECT_EQ(val, "foo");
}

TEST_CASE(spsc_try_emplace)
{
    struct Complex {
        int a;
        int b;
        Complex(int a, int b)
            : a(a)
            , b(b)
        {
        }
        Complex()
            : a(0)
            , b(0)
        {
        } // Currently required
    };

    SPSCRingBuffer<Complex, 4> buffer;
    EXPECT(buffer.try_emplace(1, 2));

    Complex val;
    EXPECT(buffer.try_pop(val));
    EXPECT_EQ(val.a, 1);
    EXPECT_EQ(val.b, 2);
}

TEST_CASE(spsc_is_empty)
{
    SPSCRingBuffer<int, 4> buffer;
    EXPECT(buffer.is_empty());
    EXPECT(buffer.try_push(1));
    EXPECT(!buffer.is_empty());
    int val;
    EXPECT(buffer.try_pop(val));
    EXPECT(buffer.is_empty());
}

TEST_CASE(spsc_threaded)
{
    static constexpr size_t ITEMS_COUNT = 100000;
    static constexpr size_t BUFFER_SIZE = 128;

    using RingBufferType = SPSCRingBuffer<int, BUFFER_SIZE>;
    auto buffer = make<RingBufferType>();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<size_t> total_consumed { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> producer_done { false };

    auto producer = Threading::Thread::construct("Producer"sv, [buffer = buffer.ptr(), &producer_done] {
        for (size_t k = 0; k < ITEMS_COUNT; ++k) {
            while (!buffer->try_push(static_cast<int>(k))) {
                AK::atomic_pause();
            }
        }
        producer_done.store(true);
        return 0;
    });

    auto consumer = Threading::Thread::construct("Consumer"sv, [buffer = buffer.ptr(), &total_consumed, &producer_done] {
        size_t consumed = 0;
        while (!producer_done.load() || consumed < ITEMS_COUNT) {
            int value;
            if (buffer->try_pop(value)) {
                EXPECT_EQ(value, static_cast<int>(consumed));
                consumed++;
            } else {
                AK::atomic_pause();
            }
        }
        total_consumed.store(consumed);
        return 0;
    });

    consumer->start();
    producer->start();

    (void)producer->join();
    (void)consumer->join();

    EXPECT_EQ(total_consumed.load(), ITEMS_COUNT);
}

BENCHMARK_CASE(spsc_throughput_batch)
{
    static constexpr size_t NUM_BATCHES = 100'000;
    static constexpr size_t BATCH_SIZE = 128;
    SPSCRingBuffer<int, 256> buffer;

    for (size_t i = 0; i < NUM_BATCHES; ++i) {
        for (size_t j = 0; j < BATCH_SIZE; ++j) {
            while (!buffer.try_push(1))
                ;
        }
        for (size_t j = 0; j < BATCH_SIZE; ++j) {
            int val;
            while (!buffer.try_pop(val))
                ;
        }
    }
}

BENCHMARK_CASE(spsc_throughput_threaded)
{
    static constexpr size_t ITEMS_COUNT = 10'000'000;
    static constexpr size_t BUFFER_SIZE = 1024;

    using RingBufferType = SPSCRingBuffer<int, BUFFER_SIZE>;
    auto buffer = make<RingBufferType>();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> producer_done { false };

    auto producer = Threading::Thread::construct("Producer"sv, [buffer = buffer.ptr(), &producer_done] {
        for (size_t k = 0; k < ITEMS_COUNT; ++k) {
            while (!buffer->try_push(static_cast<int>(k))) {
                AK::atomic_pause();
            }
        }
        producer_done.store(true);
        return 0;
    });

    auto consumer = Threading::Thread::construct("Consumer"sv, [buffer = buffer.ptr(), &producer_done] {
        size_t consumed = 0;
        while (!producer_done.load() || consumed < ITEMS_COUNT) {
            int value;
            if (buffer->try_pop(value)) {
                consumed++;
            } else {
                AK::atomic_pause();
            }
        }
        return 0;
    });

    consumer->start();
    producer->start();

    (void)producer->join();
    (void)consumer->join();
}
