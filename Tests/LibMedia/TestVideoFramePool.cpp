/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibMedia/VideoFramePool.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

static constexpr size_t FRAME_BYTE_COUNT = 4096;

static NonnullRefPtr<Media::VideoFramePool> make_pool(Function<void()> slot_freed_callback = nullptr, size_t byte_budget = Media::VideoFramePool::DEFAULT_BYTE_BUDGET)
{
    return MUST(Media::VideoFramePool::create(move(slot_freed_callback), byte_budget));
}

// The size of one allocated slot buffer, including its header, as observed through the pool's
// byte accounting.
static size_t measured_slot_buffer_size(size_t byte_count)
{
    auto pool = make_pool();
    (void)pool->try_acquire(byte_count).release_value();
    return pool->allocated_byte_count();
}

TEST_CASE(acquire_exhaustion_and_release)
{
    auto pool = make_pool();
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());

    EXPECT(!pool->try_acquire(FRAME_BYTE_COUNT).has_value());

    pool->release_hold(slots.take_last().index);
    EXPECT(pool->try_acquire(FRAME_BYTE_COUNT).has_value());
}

TEST_CASE(slot_acquisition_ids_increment_per_acquire)
{
    auto pool = make_pool();
    auto first = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    pool->release_hold(first.index);

    // Acquire until the same slot comes around again.
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++) {
        auto reacquired = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
        if (reacquired.index == first.index) {
            EXPECT_EQ(reacquired.slot_acquisition_id, first.slot_acquisition_id + 1);
            return;
        }
    }
    FAIL("Released slot was never reacquired");
}

TEST_CASE(holds_prevent_recycling)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    pool->add_hold(slot.index);

    // Dropping the adopted slot's hold still leaves the added hold outstanding.
    {
        auto adopted_slot = MUST(pool->try_adopt_acquired_slot(slot));
    }

    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT - 1; i++)
        EXPECT(pool->try_acquire(FRAME_BYTE_COUNT).has_value());
    EXPECT(!pool->try_acquire(FRAME_BYTE_COUNT).has_value());

    pool->release_hold(slot.index);
    auto reacquired = pool->try_acquire(FRAME_BYTE_COUNT);
    EXPECT(reacquired.has_value());
    EXPECT_EQ(reacquired->index, slot.index);
}

TEST_CASE(buffers_are_reused_on_exact_capacity_match)
{
    auto pool = make_pool();
    auto first = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    auto allocated_after_first = pool->allocated_byte_count();
    pool->release_hold(first.index);

    // Reacquiring the same byte count reuses the free buffer without allocating.
    auto reacquired = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    EXPECT_EQ(reacquired.index, first.index);
    EXPECT_EQ(pool->allocated_byte_count(), allocated_after_first);
    EXPECT_EQ(reacquired.slot_acquisition_id, first.slot_acquisition_id + 1);
    pool->release_hold(reacquired.index);

    // Give every slot a buffer so that a larger frame must replace one.
    Array<u64, Media::VideoFramePool::MAX_SLOT_COUNT> last_slot_acquisition_ids {};
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());
    for (auto const& acquired : slots) {
        last_slot_acquisition_ids[acquired.index] = acquired.slot_acquisition_id;
        pool->release_hold(acquired.index);
    }

    // The replacement keeps the slot's acquisition ID monotonic across buffers.
    auto larger = pool->try_acquire(FRAME_BYTE_COUNT * 2).release_value();
    EXPECT(pool->allocated_byte_count() > allocated_after_first);
    EXPECT_EQ(larger.slot_acquisition_id, last_slot_acquisition_ids[larger.index] + 1);
}

TEST_CASE(budget_always_allows_the_liveness_floor)
{
    // With a zero budget, every allocation exceeds the budget, so exactly the liveness floor
    // can be held at once.
    auto pool = make_pool(nullptr, 0);
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MIN_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());

    EXPECT(!pool->try_acquire(FRAME_BYTE_COUNT).has_value());

    pool->release_hold(slots.take_last().index);
    EXPECT(pool->try_acquire(FRAME_BYTE_COUNT).has_value());
}

TEST_CASE(excess_free_buffers_are_dropped_to_fit_the_budget)
{
    auto slot_buffer_size = measured_slot_buffer_size(FRAME_BYTE_COUNT);
    auto pool = make_pool(nullptr, slot_buffer_size * Media::VideoFramePool::MIN_SLOT_COUNT);

    // Fill the budget with free buffers.
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MIN_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());
    for (auto const& slot : slots)
        pool->release_hold(slot.index);
    EXPECT_EQ(pool->allocated_byte_count(), slot_buffer_size * Media::VideoFramePool::MIN_SLOT_COUNT);

    // A larger allocation replaces one free buffer and sheds others to return to the budget.
    auto larger = pool->try_acquire(FRAME_BYTE_COUNT * 2).release_value();
    (void)larger;
    EXPECT(pool->allocated_byte_count() <= slot_buffer_size * Media::VideoFramePool::MIN_SLOT_COUNT);
}

TEST_CASE(shed_buffers_frees_all_but_held_slots)
{
    auto pool = make_pool();
    auto held = pool->try_acquire(FRAME_BYTE_COUNT).release_value();

    auto released = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    pool->release_hold(released.index);

    pool->shed_buffers();

    // Only the held slot's buffer remains.
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(FRAME_BYTE_COUNT));

    // The held buffer is freed once its last hold releases.
    pool->release_hold(held.index);
    EXPECT_EQ(pool->allocated_byte_count(), 0u);

    // Acquiring again allocates fresh with a monotonic acquisition ID.
    auto fresh = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    EXPECT(fresh.slot_acquisition_id > held.slot_acquisition_id || fresh.index != held.index);
}

TEST_CASE(acquisition_ends_shedding_of_held_buffers)
{
    auto pool = make_pool();
    auto held = pool->try_acquire(FRAME_BYTE_COUNT).release_value();

    pool->shed_buffers();

    // An acquisition returns the pool to service, so the buffer held through the shed is
    // recycled rather than freed when its hold releases.
    auto reacquired = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    pool->release_hold(held.index);
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(FRAME_BYTE_COUNT) * 2);

    pool->release_hold(reacquired.index);
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(FRAME_BYTE_COUNT) * 2);
}

TEST_CASE(smaller_acquisitions_replace_oversized_buffers)
{
    auto pool = make_pool();
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT * 2).release_value());
    for (auto const& slot : slots)
        pool->release_hold(slot.index);

    // With every slot holding an oversized free buffer, an exact-size acquisition replaces one
    // rather than reusing the excess capacity.
    auto exact = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    EXPECT_EQ(pool->allocated_byte_count(),
        (measured_slot_buffer_size(FRAME_BYTE_COUNT * 2) * (Media::VideoFramePool::MAX_SLOT_COUNT - 1))
            + measured_slot_buffer_size(FRAME_BYTE_COUNT));
    pool->release_hold(exact.index);
}

TEST_CASE(slot_freed_callback_fires_when_a_slot_becomes_free)
{
    size_t freed_count = 0;
    auto pool = make_pool([&freed_count] { freed_count++; });

    auto slot = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    pool->add_hold(slot.index);

    // Releasing a hold that leaves the slot held does not fire.
    pool->release_hold(slot.index);
    EXPECT_EQ(freed_count, 0u);

    pool->release_hold(slot.index);
    EXPECT_EQ(freed_count, 1u);
    EXPECT(pool->try_acquire(FRAME_BYTE_COUNT).has_value());
}

TEST_CASE(slot_freed_callback_wakes_a_waiting_acquirer)
{
    // The waiter mirrors how a producer integrates the callback: it sleeps on its own
    // condition variable and retries try_acquire() on each callback-driven wake.
    struct WaiterState {
        Sync::Mutex mutex;
        Sync::ConditionVariable condition { mutex };
    };
    IGNORE_USE_IN_ESCAPING_LAMBDA WaiterState waiter;
    IGNORE_USE_IN_ESCAPING_LAMBDA auto pool = make_pool([&waiter] {
        Sync::MutexLocker locker { waiter.mutex };
        waiter.condition.broadcast();
    });

    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> started { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u32> acquired_index { 0 };
    auto acquirer_thread = Threading::Thread::construct("PoolAcquirer"sv, [&pool, &waiter, &started, &acquired_index]() {
        Sync::MutexLocker locker { waiter.mutex };
        started.store(true);
        while (true) {
            if (auto slot = pool->try_acquire(FRAME_BYTE_COUNT); slot.has_value()) {
                acquired_index.store(slot->index);
                return 0;
            }
            waiter.condition.wait();
        }
    });
    acquirer_thread->start();

    while (!started.load())
        ;
    auto const released_index = slots.take_last().index;
    pool->release_hold(released_index);

    // If the release fails to wake the waiter, this join hangs the test.
    (void)acquirer_thread->join();
    EXPECT_EQ(acquired_index.load(), released_index);
}
