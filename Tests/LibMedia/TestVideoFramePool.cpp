/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/System.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFrameHandle.h>
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

static constexpr Gfx::IntSize RESOLVED_FRAME_SIZE { 64, 64 };
static constexpr u8 RESOLVED_FRAME_BIT_DEPTH = 8;
static Media::Subsampling const RESOLVED_FRAME_SUBSAMPLING { true, true };

static size_t resolved_frame_byte_count()
{
    return MUST(Media::frame_plane_layout(RESOLVED_FRAME_SIZE, RESOLVED_FRAME_BIT_DEPTH, RESOLVED_FRAME_SUBSAMPLING)).total_byte_count;
}

static Media::VideoFrameHandle make_handle(Media::VideoFramePool const& pool, Media::VideoFramePool::AcquiredSlot const& slot)
{
    return Media::VideoFrameHandle {
        .pool_id = pool.id(),
        .slot_index = slot.index,
        .slot_acquisition_id = slot.slot_acquisition_id,
        .timestamp = AK::Duration::from_milliseconds(40),
        .duration = AK::Duration::from_milliseconds(20),
        .size = RESOLVED_FRAME_SIZE,
        .bit_depth = RESOLVED_FRAME_BIT_DEPTH,
        .subsampling = RESOLVED_FRAME_SUBSAMPLING,
        .cicp = {
            Media::ColorPrimaries::BT709,
            Media::TransferCharacteristics::BT709,
            Media::MatrixCoefficients::BT709,
            Media::VideoFullRangeFlag::Full,
        },
    };
}

static Media::VideoFrameHandle make_stress_handle(Media::VideoFramePoolID pool_id, u32 slot_index, u64 slot_acquisition_id)
{
    auto handle = Media::VideoFrameHandle {
        .pool_id = pool_id,
        .slot_index = slot_index,
        .slot_acquisition_id = slot_acquisition_id,
        .timestamp = AK::Duration::zero(),
        .duration = AK::Duration::zero(),
        .size = RESOLVED_FRAME_SIZE,
        .bit_depth = RESOLVED_FRAME_BIT_DEPTH,
        .subsampling = RESOLVED_FRAME_SUBSAMPLING,
        .cicp = {
            Media::ColorPrimaries::BT709,
            Media::TransferCharacteristics::BT709,
            Media::MatrixCoefficients::BT709,
            Media::VideoFullRangeFlag::Full,
        },
    };
    return handle;
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
    auto held = pool->try_acquire(resolved_frame_byte_count()).release_value();
    held.bytes.fill(0x42);
    auto directory = Media::VideoFrameSlotDirectory::create();
    directory->notify_slot_announced(pool->id(), held.index, pool->slot_buffer(held.index));

    auto released = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    pool->release_hold(released.index);

    pool->shed_buffers();

    // Only the held slot's buffer remains, and it still resolves.
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(resolved_frame_byte_count()));
    auto frame = directory->resolve_frame(make_handle(pool, held), [] { });
    EXPECT(frame != nullptr);
    EXPECT_EQ(frame->yuv_data().y_data()[0], 0x42);
    EXPECT(frame->revalidate_backing());

    // The held buffer is freed once its last hold releases, and the directory's mapping
    // remains readable.
    pool->release_hold(held.index);
    EXPECT_EQ(pool->allocated_byte_count(), 0u);
    EXPECT(directory->resolve_frame(make_handle(pool, held), [] { }) != nullptr);

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

TEST_CASE(directory_resolves_the_current_slot_acquisition_only)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    slot.bytes.fill(0x42);

    auto directory = Media::VideoFrameSlotDirectory::create();
    directory->notify_slot_announced(pool->id(), slot.index, pool->slot_buffer(slot.index));

    auto frame = directory->resolve_frame(make_handle(pool, slot), [] { });
    EXPECT(frame != nullptr);
    EXPECT_EQ(frame->yuv_data().y_data()[0], 0x42);
    EXPECT(frame->revalidate_backing());

    // Recycling the slot into the same buffer invalidates the old acquisition ID, and the new
    // acquisition resolves without a fresh announcement.
    pool->release_hold(slot.index);
    Optional<Media::VideoFramePool::AcquiredSlot> reacquired;
    while (true) {
        reacquired = pool->try_acquire(resolved_frame_byte_count());
        if (reacquired->index == slot.index)
            break;
        pool->release_hold(reacquired->index);
    }
    EXPECT(!frame->revalidate_backing());
    EXPECT(directory->resolve_frame(make_handle(pool, slot), [] { }) == nullptr);
    EXPECT(directory->resolve_frame(make_handle(pool, *reacquired), [] { }) != nullptr);
}

TEST_CASE(replaced_buffers_stay_resolvable_through_old_mappings)
{
    auto pool = make_pool();

    // Give every slot a buffer so that a larger frame must replace one.
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(resolved_frame_byte_count()).release_value());
    auto slot = slots.first();
    slot.bytes.fill(0x17);
    auto directory = Media::VideoFrameSlotDirectory::create();
    directory->notify_slot_announced(pool->id(), slot.index, pool->slot_buffer(slot.index));
    for (auto const& acquired : slots)
        pool->release_hold(acquired.index);

    // A larger acquisition replaces the slot's buffer. The old buffer is never written again, so
    // the directory's mapping still resolves the old acquisition ID.
    auto replaced = pool->try_acquire(resolved_frame_byte_count() * 4).release_value();
    EXPECT_EQ(replaced.index, slot.index);
    auto old_frame = directory->resolve_frame(make_handle(pool, slot), [] { });
    EXPECT(old_frame != nullptr);
    EXPECT_EQ(old_frame->yuv_data().y_data()[0], 0x17);
    EXPECT(old_frame->revalidate_backing());

    // The new acquisition resolves only once its buffer is announced.
    EXPECT(directory->resolve_frame(make_handle(pool, replaced), [] { }) == nullptr);
    directory->notify_slot_announced(pool->id(), replaced.index, pool->slot_buffer(replaced.index));
    EXPECT(directory->resolve_frame(make_handle(pool, replaced), [] { }) != nullptr);
    EXPECT(directory->resolve_frame(make_handle(pool, slot), [] { }) == nullptr);
}

TEST_CASE(directory_resolves_through_a_second_mapping)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    slot.bytes.fill(0x7f);

    auto slot_buffer = pool->slot_buffer(slot.index);
    auto transferred_fd = MUST(Core::System::dup(slot_buffer.fd()));
    auto transferred_buffer = MUST(Core::AnonymousBuffer::create_from_anon_fd(transferred_fd, slot_buffer.size()));
    auto directory = Media::VideoFrameSlotDirectory::create();
    directory->notify_slot_announced(pool->id(), slot.index, transferred_buffer);

    auto frame = directory->resolve_frame(make_handle(pool, slot), [] { });
    EXPECT(frame != nullptr);
    EXPECT_NE(frame->yuv_data().y_data().data(), static_cast<u8 const*>(slot.bytes.data()));
    EXPECT_EQ(frame->yuv_data().y_data()[0], 0x7f);
}

TEST_CASE(directory_ignores_invalid_slot_buffers)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();

    auto directory = Media::VideoFrameSlotDirectory::create();
    size_t slots_changed_count = 0;
    directory->set_on_slots_changed([&] { slots_changed_count++; });

    directory->notify_slot_announced(pool->id(), slot.index, {});
    auto too_small = MUST(Core::AnonymousBuffer::create_with_size(16));
    directory->notify_slot_announced(pool->id(), slot.index, too_small);

    EXPECT_EQ(slots_changed_count, 0u);
    EXPECT(directory->resolve_frame(make_handle(pool, slot), [] { }) == nullptr);
}

TEST_CASE(pool_ids_are_unique)
{
    EXPECT_NE(make_pool()->id(), make_pool()->id());
}

TEST_CASE(resolved_frames_read_slots_and_release_on_destruction)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    slot.bytes.fill(0x33);
    auto directory = Media::VideoFrameSlotDirectory::create();
    directory->notify_slot_announced(pool->id(), slot.index, pool->slot_buffer(slot.index));

    bool released = false;
    {
        auto frame = directory->resolve_frame(make_handle(pool, slot), [&] { released = true; });
        EXPECT(frame != nullptr);
        EXPECT_EQ(frame->yuv_data().y_data()[0], 0x33);
        EXPECT_EQ(frame->timestamp(), AK::Duration::from_milliseconds(40));
        EXPECT(frame->revalidate_backing());
        EXPECT(frame->pool_slot() == nullptr);
        EXPECT(!released);
    }
    EXPECT(released);
}

TEST_CASE(resolve_frame_rejects_stale_and_oversized_handles)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    auto directory = Media::VideoFrameSlotDirectory::create();
    directory->notify_slot_announced(pool->id(), slot.index, pool->slot_buffer(slot.index));

    // A format too large for the slot's buffer must not resolve.
    auto oversized = make_handle(pool, slot);
    oversized.bit_depth = 10;
    EXPECT(directory->resolve_frame(oversized, [] { }) == nullptr);

    auto handle = make_handle(pool, slot);
    auto frame = directory->resolve_frame(handle, [] { });
    EXPECT(frame != nullptr);

    // Recycle the slot; the resolved frame must fail revalidation, and the stale handle must
    // no longer resolve.
    pool->release_hold(slot.index);
    while (true) {
        auto reacquired = pool->try_acquire(resolved_frame_byte_count()).release_value();
        if (reacquired.index == slot.index)
            break;
        pool->release_hold(reacquired.index);
    }
    EXPECT(!frame->revalidate_backing());
    EXPECT(directory->resolve_frame(handle, [] { }) == nullptr);
}

TEST_CASE(directory_resolves_announced_slots)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    slot.bytes.fill(0x66);

    auto directory = Media::VideoFrameSlotDirectory::create();
    size_t slots_changed_count = 0;
    directory->set_on_slots_changed([&] { slots_changed_count++; });

    // A handle resolves to nothing until its slot has been announced.
    auto handle = make_handle(pool, slot);
    EXPECT(directory->resolve_frame(handle, [] { }) == nullptr);

    directory->notify_slot_announced(pool->id(), slot.index, pool->slot_buffer(slot.index));
    EXPECT_EQ(slots_changed_count, 1u);

    bool released = false;
    {
        auto frame = directory->resolve_frame(handle, [&] { released = true; });
        EXPECT(frame != nullptr);
        EXPECT_EQ(frame->yuv_data().y_data()[0], 0x66);
        EXPECT(!released);
    }
    EXPECT(released);

    directory->notify_pool_retired(pool->id());
    EXPECT(directory->resolve_frame(handle, [] { }) == nullptr);
}

TEST_CASE(recycle_versus_hold_stress)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto pool = make_pool();
    auto const frame_count = 50'000;

    // Allocate every slot's buffer up front so the consumer's mappings stay valid while the
    // producer recycles same-sized frames through them.
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(resolved_frame_byte_count()).release_value());
    IGNORE_USE_IN_ESCAPING_LAMBDA auto directory = Media::VideoFrameSlotDirectory::create();
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        directory->notify_slot_announced(pool->id(), i, pool->slot_buffer(i));
    for (auto const& slot : slots)
        pool->release_hold(slot.index);

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u64> published_packed { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> producer_done { false };

    IGNORE_USE_IN_ESCAPING_LAMBDA auto pool_id = pool->id();
    auto consumer_thread = Threading::Thread::construct("PoolConsumer"sv, [&directory, &pool_id, &published_packed, &producer_done]() {
        size_t verified_count = 0;
        auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(30);
        while ((!producer_done.load() || verified_count == 0) && MonotonicTime::now_coarse() < deadline) {
            auto packed = published_packed.load();
            if (packed == 0)
                continue;
            // The producer packs (slot << 32) | (slot_acquisition_id & 0xffffffff), with slot_acquisition_id
            // also determining the fill byte, so any undetected recycle shows up as a
            // mismatched pattern below.
            auto slot_index = static_cast<u32>(packed >> 32);
            auto slot_acquisition_id = packed & 0xffffffffu;

            auto handle = make_stress_handle(pool_id, slot_index, slot_acquisition_id);
            auto frame = directory->resolve_frame(handle, [] { });
            if (frame == nullptr)
                continue;
            auto first = frame->yuv_data().y_data()[0];
            auto last = frame->yuv_data().v_data()[frame->yuv_data().v_data().size() - 1];
            if (!frame->revalidate_backing())
                continue;
            // Revalidation passed, so the bytes we read must match this slot_acquisition_id's pattern.
            EXPECT_EQ(first, static_cast<u8>(slot_acquisition_id));
            EXPECT_EQ(last, static_cast<u8>(slot_acquisition_id));
            verified_count++;
        }
        EXPECT(verified_count > 0);
        return 0;
    });
    consumer_thread->start();

    for (int i = 0; i < frame_count; i++) {
        auto slot = pool->try_acquire(resolved_frame_byte_count());
        if (!slot.has_value())
            continue;
        VERIFY(slot->slot_acquisition_id <= 0xffffffffu);
        slot->bytes.fill(static_cast<u8>(slot->slot_acquisition_id));
        published_packed.store((static_cast<u64>(slot->index) << 32) | slot->slot_acquisition_id);
        pool->release_hold(slot->index);
    }
    producer_done.store(true);
    (void)consumer_thread->join();
}
