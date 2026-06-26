/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/SeqLock.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

namespace {

// Large enough that a torn read would expose a partially-updated value.
struct Payload {
    u64 values[16];
};

}

TEST_CASE(store_then_read)
{
    AK::SeqLock<Payload> lock;
    Payload written;
    for (auto& value : written.values)
        value = 0xABCD;
    lock.store(written);

    auto snapshot = lock.read();
    EXPECT(snapshot.has_value());
    for (auto value : snapshot->values)
        EXPECT_EQ(value, 0xABCDu);
}

TEST_CASE(writes_can_modify_the_previous_value)
{
    AK::SeqLock<Payload> lock;
    lock.write([](TearableAtomic<Payload>& slot) {
        Payload payload {};
        for (auto& value : payload.values)
            value = 1;
        slot.store_relaxed(payload);
    });
    lock.write([](TearableAtomic<Payload>& slot) {
        auto payload = slot.load_relaxed();
        for (auto& value : payload.values)
            value += 41;
        slot.store_relaxed(payload);
    });

    auto snapshot = lock.read();
    EXPECT(snapshot.has_value());
    for (auto value : snapshot->values)
        EXPECT_EQ(value, 42u);
}

// A reader racing a continuous writer must always observe a self-consistent snapshot (all fields
// from one write), never a mixture of two writes.
TEST_CASE(concurrent_reads_are_never_torn)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA AK::SeqLock<Payload> lock;
    lock.store({});
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> stop { false };

    auto writer = Threading::Thread::construct("SeqLockWriter"sv, [&lock, &stop]() {
        for (u64 counter = 1; !stop.load(AK::MemoryOrder::memory_order_relaxed); counter++) {
            Payload payload {};
            for (auto& value : payload.values)
                value = counter;
            lock.write([&payload](TearableAtomic<Payload>& slot) {
                slot.store_relaxed(payload);
            });
        }
        return 0;
    });
    writer->start();

    size_t successful_reads = 0;
    for (size_t iteration = 0; iteration < 500'000; iteration++) {
        auto snapshot = lock.read();
        if (!snapshot.has_value())
            continue;
        successful_reads++;
        auto expected = snapshot->values[0];
        for (auto value : snapshot->values)
            EXPECT_EQ(value, expected);
    }
    EXPECT(successful_reads > 0);

    stop.store(true, AK::MemoryOrder::memory_order_relaxed);
    (void)writer->join();
}

// weak_read() skips the protocol and is only valid on the writer thread; it returns the last write.
TEST_CASE(weak_read_returns_the_last_written_value)
{
    AK::SeqLock<Payload> lock;
    lock.write([](TearableAtomic<Payload>& slot) {
        Payload payload {};
        for (auto& value : payload.values)
            value = 67;
        slot.store_relaxed(payload);
    });

    auto snapshot = lock.weak_read();
    for (auto value : snapshot.values)
        EXPECT_EQ(value, 67u);
}
