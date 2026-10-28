/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConditionVariable.h>
#include <AK/MemoryStream.h>
#include <AK/Mutex.h>
#include <AK/Queue.h>
#include <AK/Random.h>
#include <AK/Time.h>
#include <LibCore/System.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/Limits.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFrameMetadata.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibMediaCodecs/VideoSurface.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

static constexpr size_t FRAME_BYTE_COUNT = 4096;

static NonnullRefPtr<Media::VideoFramePool> make_pool(Function<void()> slot_freed_callback = nullptr, size_t byte_budget = Media::VideoFramePool::DEFAULT_BYTE_BUDGET)
{
    auto pool = MUST(Media::VideoFramePool::create(byte_budget));
    pool->set_slot_freed_callback(move(slot_freed_callback));
    return pool;
}

// The size of one allocated slot buffer, as observed through the pool's byte accounting.
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

static Media::VideoFrameMetadata make_metadata()
{
    return Media::VideoFrameMetadata {
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

// Maps a slot's buffer a second time, as receiving it in another process would.
static Core::AnonymousBuffer received_copy_of(Core::AnonymousBuffer const& buffer)
{
    return MUST(Core::AnonymousBuffer::create_from_anon_fd(MUST(Core::System::dup(buffer.fd())), buffer.size()));
}

static bool buffers_share_memory(Core::AnonymousBuffer const& a, Core::AnonymousBuffer const& b)
{
    return a.is_valid() && b.is_valid() && a.data<u8 const>() == b.data<u8 const>();
}

TEST_CASE(acquire_exhaustion_and_release)
{
    auto pool = make_pool();
    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());

    EXPECT(!pool->try_acquire(FRAME_BYTE_COUNT).has_value());

    slots.take_last();
    EXPECT(pool->try_acquire(FRAME_BYTE_COUNT).has_value());
}

TEST_CASE(a_slot_stays_held_while_any_frame_shares_its_storage)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(FRAME_BYTE_COUNT);
    RefPtr<Media::VideoFrameStorage> second_reference = slot->storage;
    auto slot_buffer = slot->storage->pixel_buffer();
    slot.clear();

    Vector<Media::VideoFramePool::AcquiredSlot> other_slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT - 1; i++)
        other_slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());
    EXPECT(!pool->try_acquire(FRAME_BYTE_COUNT).has_value());

    second_reference = nullptr;
    auto reacquired = pool->try_acquire(FRAME_BYTE_COUNT);
    EXPECT(reacquired.has_value());
    EXPECT(buffers_share_memory(reacquired->storage->pixel_buffer(), slot_buffer));
}

TEST_CASE(buffers_are_reused_on_exact_capacity_match)
{
    auto pool = make_pool();
    auto first = pool->try_acquire(FRAME_BYTE_COUNT);
    auto first_buffer = first->storage->pixel_buffer();
    auto allocated_after_first = pool->allocated_byte_count();
    first.clear();

    // Reacquiring the same byte count reuses the free buffer without allocating.
    auto reacquired = pool->try_acquire(FRAME_BYTE_COUNT);
    EXPECT(buffers_share_memory(reacquired->storage->pixel_buffer(), first_buffer));
    EXPECT_EQ(pool->allocated_byte_count(), allocated_after_first);
    reacquired.clear();

    // A different byte count needs a buffer of its own.
    auto larger = pool->try_acquire(FRAME_BYTE_COUNT * 2).release_value();
    EXPECT(pool->allocated_byte_count() > allocated_after_first);
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

    slots.take_last();
    EXPECT(pool->try_acquire(FRAME_BYTE_COUNT).has_value());
}

TEST_CASE(excess_free_buffers_are_dropped_to_fit_the_budget)
{
    auto slot_buffer_size = measured_slot_buffer_size(FRAME_BYTE_COUNT);
    auto pool = make_pool(nullptr, slot_buffer_size * Media::VideoFramePool::MIN_SLOT_COUNT);

    // Fill the budget with free buffers.
    {
        Vector<Media::VideoFramePool::AcquiredSlot> slots;
        for (u32 i = 0; i < Media::VideoFramePool::MIN_SLOT_COUNT; i++)
            slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());
    }
    EXPECT_EQ(pool->allocated_byte_count(), slot_buffer_size * Media::VideoFramePool::MIN_SLOT_COUNT);

    // A larger allocation replaces one free buffer and sheds others to return to the budget.
    auto larger = pool->try_acquire(FRAME_BYTE_COUNT * 2).release_value();
    EXPECT(pool->allocated_byte_count() <= slot_buffer_size * Media::VideoFramePool::MIN_SLOT_COUNT);
}

TEST_CASE(shed_storage_frees_all_but_held_slots)
{
    auto pool = make_pool();
    auto held = pool->try_acquire(resolved_frame_byte_count());
    held->bytes.fill(0x42);
    auto frame = MUST(Media::create_received_video_frame(make_metadata(), received_copy_of(held->storage->pixel_buffer()), 0, [] { }));

    (void)pool->try_acquire(FRAME_BYTE_COUNT).release_value();

    pool->shed_storage();

    // Only the held slot's buffer remains.
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(resolved_frame_byte_count()));
    EXPECT_EQ(frame->yuv_data()->y_data()[0], 0x42);

    // The held buffer is freed once its last hold releases, while a frame received from it keeps reading it.
    held.clear();
    EXPECT_EQ(pool->allocated_byte_count(), 0u);
    EXPECT_EQ(frame->yuv_data()->y_data()[0], 0x42);
}

TEST_CASE(acquisition_ends_shedding_of_held_buffers)
{
    auto pool = make_pool();
    auto held = pool->try_acquire(FRAME_BYTE_COUNT);

    pool->shed_storage();

    // An acquisition returns the pool to service, so the buffer held through the shed is
    // recycled rather than freed when its hold releases.
    auto reacquired = pool->try_acquire(FRAME_BYTE_COUNT);
    held.clear();
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(FRAME_BYTE_COUNT) * 2);

    reacquired.clear();
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(FRAME_BYTE_COUNT) * 2);
}

TEST_CASE(smaller_acquisitions_replace_oversized_buffers)
{
    auto pool = make_pool();
    {
        Vector<Media::VideoFramePool::AcquiredSlot> slots;
        for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
            slots.append(pool->try_acquire(FRAME_BYTE_COUNT * 2).release_value());
    }

    // With every slot holding an oversized free buffer, an exact-size acquisition replaces one
    // rather than reusing the excess capacity.
    auto exact = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    EXPECT_EQ(pool->allocated_byte_count(),
        (measured_slot_buffer_size(FRAME_BYTE_COUNT * 2) * (Media::VideoFramePool::MAX_SLOT_COUNT - 1))
            + measured_slot_buffer_size(FRAME_BYTE_COUNT));
}

TEST_CASE(slot_freed_callback_fires_when_a_slot_becomes_free)
{
    size_t freed_count = 0;
    auto pool = make_pool([&freed_count] { freed_count++; });

    auto slot = pool->try_acquire(FRAME_BYTE_COUNT);
    RefPtr<Media::VideoFrameStorage> second_reference = slot->storage;

    // Dropping a reference that leaves the slot held does not fire.
    slot.clear();
    EXPECT_EQ(freed_count, 0u);

    second_reference = nullptr;
    EXPECT_EQ(freed_count, 1u);
    EXPECT(pool->try_acquire(FRAME_BYTE_COUNT).has_value());
}

TEST_CASE(slot_freed_callback_wakes_a_waiting_acquirer)
{
    // The waiter mirrors how a producer integrates the callback: it sleeps on its own
    // condition variable and retries try_acquire() on each callback-driven wake.
    struct WaiterState {
        Mutex mutex;
        ConditionVariable condition { mutex };
    };
    IGNORE_USE_IN_ESCAPING_LAMBDA WaiterState waiter;
    IGNORE_USE_IN_ESCAPING_LAMBDA auto pool = make_pool([&waiter] {
        MutexLocker locker { waiter.mutex };
        waiter.condition.broadcast();
    });

    Vector<Media::VideoFramePool::AcquiredSlot> slots;
    for (u32 i = 0; i < Media::VideoFramePool::MAX_SLOT_COUNT; i++)
        slots.append(pool->try_acquire(FRAME_BYTE_COUNT).release_value());

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> started { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Mutex acquired_mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<Media::VideoFramePool::AcquiredSlot> acquired;
    auto acquirer_thread = Threading::Thread::construct("PoolAcquirer"sv, [&pool, &waiter, &started, &acquired_mutex, &acquired]() {
        MutexLocker locker { waiter.mutex };
        started.store(true);
        while (true) {
            if (auto slot = pool->try_acquire(FRAME_BYTE_COUNT); slot.has_value()) {
                MutexLocker acquired_locker { acquired_mutex };
                acquired = slot.release_value();
                return 0;
            }
            waiter.condition.wait();
        }
    });
    acquirer_thread->start();

    while (!started.load())
        ;
    auto released_buffer = slots.last().storage->pixel_buffer();
    slots.take_last();

    // If the release fails to wake the waiter, this join hangs the test.
    (void)acquirer_thread->join();
    MutexLocker acquired_locker { acquired_mutex };
    EXPECT(buffers_share_memory(acquired->storage->pixel_buffer(), released_buffer));
}

TEST_CASE(received_frames_read_their_buffer_and_release_it_once_dropped)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    slot.bytes.fill(0x33);

    bool released = false;
    {
        auto frame = MUST(Media::create_received_video_frame(make_metadata(), received_copy_of(slot.storage->pixel_buffer()), 7, [&] { released = true; }));
        EXPECT_NE(frame->yuv_data()->y_data().data(), static_cast<u8 const*>(slot.bytes.data()));
        EXPECT_EQ(frame->yuv_data()->y_data()[0], 0x33);
        EXPECT_EQ(frame->timestamp(), AK::Duration::from_milliseconds(40));
        EXPECT_EQ(frame->storage()->sender_id(), 7u);
        EXPECT(!released);
    }
    EXPECT(released);
}

// The storage comes from another process, so a frame it could not hold is refused, and the storage released at once.
TEST_CASE(received_frames_must_fit_their_storage)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();

    auto release_count = 0u;
    auto oversized = make_metadata();
    oversized.bit_depth = 10;
    EXPECT(Media::create_received_video_frame(oversized, received_copy_of(slot.storage->pixel_buffer()), 0, [&] { release_count++; }).is_error());
    EXPECT_EQ(release_count, 1u);

    auto too_small = MUST(Core::AnonymousBuffer::create_with_size(16));
    EXPECT(Media::create_received_video_frame(make_metadata(), too_small, 0, [&] { release_count++; }).is_error());
    EXPECT_EQ(release_count, 2u);

    EXPECT(Media::create_received_video_frame(make_metadata(), Core::AnonymousBuffer {}, 0, [&] { release_count++; }).is_error());
    EXPECT_EQ(release_count, 3u);
}

// Metadata comes from another process, so metadata describing a frame no decoder produces is refused, even when the
// storage could hold it.
TEST_CASE(received_frames_must_be_ones_a_decoder_could_produce)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(MUST(Media::frame_plane_layout(RESOLVED_FRAME_SIZE, 12, Media::Subsampling::yuv444())).total_byte_count).release_value();
    auto receive = [&](Media::VideoFrameMetadata const& metadata) {
        return Media::create_received_video_frame(metadata, received_copy_of(slot.storage->pixel_buffer()), 0, [] { });
    };
    EXPECT(!receive(make_metadata()).is_error());

    auto thirteen_bit = make_metadata();
    thirteen_bit.bit_depth = 13;
    EXPECT(receive(thirteen_bit).is_error());

    auto negative_duration = make_metadata();
    negative_duration.duration = AK::Duration::from_milliseconds(-20);
    EXPECT(receive(negative_duration).is_error());

    auto unknown_primaries = make_metadata();
    unknown_primaries.cicp.set_color_primaries(static_cast<Media::ColorPrimaries>(200));
    EXPECT(receive(unknown_primaries).is_error());

    auto too_wide = make_metadata();
    too_wide.size = { static_cast<int>(Media::MAX_VIDEO_FRAME_DIMENSION) + 1, 1 };
    EXPECT(!too_wide.is_valid());
}

TEST_CASE(metadata_that_could_not_have_been_decoded_is_refused_over_ipc)
{
    auto metadata = make_metadata();
    metadata.bit_depth = 13;

    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(metadata));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decoder.decode<Media::VideoFrameMetadata>().is_error());
}

#if defined(AK_OS_MACOS) || defined(AK_OS_LINUX)

// Stands in for what a decoder gives with a surface to keep its pixels in place, counting how many are alive.
class TestSurfaceContents final : public Media::VideoSurfaceContents {
public:
    explicit TestSurfaceContents(Atomic<u32>& live_count)
        : m_live_count(live_count)
    {
        m_live_count++;
    }
    virtual ~TestSurfaceContents() override { m_live_count--; }

private:
    Atomic<u32>& m_live_count;
};

static NonnullRefPtr<Media::VideoSurface> make_surface()
{
#    ifdef AK_OS_MACOS
    return MUST(Media::VideoSurface::create(Core::IOSurfaceHandle::create(16, 16)));
#    else
    Gfx::LinuxDmaBufHandle dma_buf;
    dma_buf.size = { 16, 16 };
    return MUST(Media::VideoSurface::create_from_dma_buf(get_random<u64>(), move(dma_buf)));
#    endif
}

static NonnullRefPtr<Media::VideoSurfaceContents> make_contents(Atomic<u32>& live_count)
{
    return make_ref_counted<TestSurfaceContents>(live_count);
}

static NonnullRefPtr<Media::VideoSurfaceContents> make_contents()
{
    static Atomic<u32> s_unwatched_live_count { 0 };
    return make_contents(s_unwatched_live_count);
}

TEST_CASE(distinct_surfaces_have_distinct_ids)
{
    auto first_surface = make_surface();
    auto second_surface = make_surface();
    EXPECT_NE(first_surface->id(), second_surface->id());
}

// The decoder recycles surfaces it is no longer kept from using, and may free them after a while, so the budget lets
// go of a surface and its contents along with the last frame holding them.
TEST_CASE(surface_budget_lets_go_of_a_surface_with_its_last_hold)
{
    Atomic<u32> live_contents { 0 };
    auto budget = make_ref_counted<Media::VideoSurfaceBudget>();
    auto surface = make_surface();

    auto storage = budget->try_hold(surface, make_contents(live_contents));
    EXPECT_NE(storage, nullptr);
    EXPECT_EQ(storage->surface()->id(), surface->id());
    EXPECT_EQ(live_contents.load(), 1u);

    auto second_reference = storage;
    storage = nullptr;
    EXPECT_EQ(live_contents.load(), 1u);

    second_reference = nullptr;
    EXPECT_EQ(live_contents.load(), 0u);
    EXPECT_EQ(surface->ref_count(), 1u);
}

TEST_CASE(surface_budget_refuses_to_exceed_its_held_surface_count)
{
    auto budget = make_ref_counted<Media::VideoSurfaceBudget>();

    // The platform allocator hands out surfaces without limit, so the budget is what stops a decoder from running
    // arbitrarily far ahead of whoever is displaying its frames.
    Vector<NonnullRefPtr<Media::VideoFrameStorage>> held;
    for (u32 index = 0; index < Media::VideoSurfaceBudget::MAX_HELD_SURFACE_COUNT; index++)
        held.append(budget->try_hold(make_surface(), make_contents()).release_nonnull());

    auto surface_too_many = make_surface();
    EXPECT_EQ(budget->try_hold(surface_too_many, make_contents()), nullptr);
    // A held surface handed back again counts as another frame in flight.
    EXPECT_EQ(budget->try_hold(held.first()->surface().release_nonnull(), make_contents()), nullptr);

    held.remove(0);
    EXPECT_NE(budget->try_hold(surface_too_many, make_contents()), nullptr);
}

#    ifdef AK_OS_MACOS
TEST_CASE(surfaces_survive_a_mach_port_round_trip)
{
    auto surface = make_surface();

    auto reimported = MUST(Core::IOSurfaceHandle::from_mach_port(surface->create_mach_port()));
    EXPECT_EQ(static_cast<u64>(reimported.id()), surface->id());
    EXPECT_EQ(reimported.width(), 16u);
    EXPECT_EQ(reimported.height(), 16u);
}
#    endif

TEST_CASE(received_surface_frames_read_their_surface)
{
    auto surface = make_surface();
    auto metadata = make_metadata();
    metadata.size = { 16, 16 };

    auto frame = MUST(Media::create_received_video_frame(metadata, surface, 0, [] { }));
    EXPECT_EQ(frame->surface()->id(), surface->id());
    // The planes of a surface-backed frame are in the surface, so no view into a buffer is offered.
    EXPECT(!frame->yuv_data().has_value());

    auto larger_than_the_surface = metadata;
    larger_than_the_surface.size = { 32, 16 };
    EXPECT(Media::create_received_video_frame(larger_than_the_surface, surface, 0, [] { }).is_error());
}

TEST_CASE(surface_budget_reports_each_released_surface)
{
    auto freed_count = 0u;
    auto budget = make_ref_counted<Media::VideoSurfaceBudget>();
    budget->set_storage_freed_callback([&] { freed_count++; });
    auto storage = budget->try_hold(make_surface(), make_contents());

    auto second_reference = storage;
    storage = nullptr;
    EXPECT_EQ(freed_count, 0u);

    second_reference = nullptr;
    EXPECT_EQ(freed_count, 1u);
}

#endif
