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
#include <LibMediaCodecs/VideoFrameHandle.h>
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

TEST_CASE(shed_storage_frees_all_but_held_slots)
{
    auto pool = make_pool();
    auto held = pool->try_acquire(resolved_frame_byte_count()).release_value();
    held.bytes.fill(0x42);
    auto frame = MUST(Media::create_received_video_frame(make_metadata(), received_copy_of(pool->slot_buffer(held.index)), nullptr, 0, [] { }));

    auto released = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    pool->release_hold(released.index);

    pool->shed_storage();

    // Only the held slot's buffer remains.
    EXPECT_EQ(pool->allocated_byte_count(), measured_slot_buffer_size(resolved_frame_byte_count()));
    EXPECT_EQ(frame->yuv_data()->y_data()[0], 0x42);

    // The held buffer is freed once its last hold releases, while a frame received from it keeps reading it.
    pool->release_hold(held.index);
    EXPECT_EQ(pool->allocated_byte_count(), 0u);
    EXPECT_EQ(frame->yuv_data()->y_data()[0], 0x42);

    // Acquiring again allocates fresh with a monotonic acquisition ID.
    auto fresh = pool->try_acquire(FRAME_BYTE_COUNT).release_value();
    EXPECT(fresh.slot_acquisition_id > held.slot_acquisition_id || fresh.index != held.index);
}

TEST_CASE(acquisition_ends_shedding_of_held_buffers)
{
    auto pool = make_pool();
    auto held = pool->try_acquire(FRAME_BYTE_COUNT).release_value();

    pool->shed_storage();

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
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u32> acquired_index { 0 };
    auto acquirer_thread = Threading::Thread::construct("PoolAcquirer"sv, [&pool, &waiter, &started, &acquired_index]() {
        MutexLocker locker { waiter.mutex };
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

TEST_CASE(pool_ids_are_unique)
{
    EXPECT_NE(make_pool()->id(), make_pool()->id());
}

TEST_CASE(received_frames_read_their_buffer_and_release_it_once_dropped)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    slot.bytes.fill(0x33);

    bool released = false;
    {
        auto frame = MUST(Media::create_received_video_frame(make_metadata(), received_copy_of(pool->slot_buffer(slot.index)), nullptr, 7, [&] { released = true; }));
        EXPECT_NE(frame->yuv_data()->y_data().data(), static_cast<u8 const*>(slot.bytes.data()));
        EXPECT_EQ(frame->yuv_data()->y_data()[0], 0x33);
        EXPECT_EQ(frame->timestamp(), AK::Duration::from_milliseconds(40));
        EXPECT(frame->pool_slot() == nullptr);
        EXPECT_EQ(frame->received_storage()->sender_id(), 7u);
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
    EXPECT(Media::create_received_video_frame(oversized, received_copy_of(pool->slot_buffer(slot.index)), nullptr, 0, [&] { release_count++; }).is_error());
    EXPECT_EQ(release_count, 1u);

    auto too_small = MUST(Core::AnonymousBuffer::create_with_size(16));
    EXPECT(Media::create_received_video_frame(make_metadata(), too_small, nullptr, 0, [&] { release_count++; }).is_error());
    EXPECT_EQ(release_count, 2u);

    EXPECT(Media::create_received_video_frame(make_metadata(), {}, nullptr, 0, [&] { release_count++; }).is_error());
    EXPECT_EQ(release_count, 3u);
}

// Metadata comes from another process, so metadata describing a frame no decoder produces is refused, even when the
// storage could hold it.
TEST_CASE(received_frames_must_be_ones_a_decoder_could_produce)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(MUST(Media::frame_plane_layout(RESOLVED_FRAME_SIZE, 12, Media::Subsampling::yuv444())).total_byte_count).release_value();
    auto receive = [&](Media::VideoFrameMetadata const& metadata) {
        return Media::create_received_video_frame(metadata, received_copy_of(pool->slot_buffer(slot.index)), nullptr, 0, [] { });
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

TEST_CASE(handles_that_could_not_have_been_decoded_are_refused_over_ipc)
{
    auto pool = make_pool();
    auto slot = pool->try_acquire(resolved_frame_byte_count()).release_value();
    auto handle = make_handle(pool, slot);
    handle.bit_depth = 13;

    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(handle));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decoder.decode<Media::VideoFrameHandle>().is_error());
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

TEST_CASE(surface_pool_keeps_a_recycled_surface_on_its_slot)
{
    auto pool = MUST(Media::VideoFrameSurfacePool::create());
    auto surface = make_surface();

    auto first = pool->try_acquire(surface, make_contents()).value();
    pool->release_hold(first.index);
    auto second = pool->try_acquire(surface, make_contents()).value();

    // Announcement is keyed on the slot's buffer generation, so a recycled surface must not appear to be a new one.
    EXPECT_EQ(second.index, first.index);
    EXPECT_EQ(second.allocated_buffer_id, first.allocated_buffer_id);
    EXPECT_NE(second.slot_acquisition_id, first.slot_acquisition_id);
}

TEST_CASE(surface_pool_drops_the_contents_with_the_last_hold)
{
    Atomic<u32> live_contents { 0 };
    auto pool = MUST(Media::VideoFrameSurfacePool::create());
    auto surface = make_surface();

    auto first = pool->try_acquire(surface, make_contents(live_contents)).value();
    EXPECT_EQ(live_contents.load(), 1u);

    pool->add_hold(first.index);
    pool->release_hold(first.index);
    EXPECT_EQ(live_contents.load(), 1u);

    // The decoder is free to decode into the surface again, which the slot recognizes it for having done because
    // it kept the surface through the release.
    pool->release_hold(first.index);
    EXPECT_EQ(live_contents.load(), 0u);

    auto second = pool->try_acquire(surface, make_contents(live_contents)).value();
    EXPECT_EQ(live_contents.load(), 1u);
    EXPECT_EQ(second.index, first.index);
    EXPECT_EQ(second.allocated_buffer_id, first.allocated_buffer_id);
}

// A decoder can hand a surface back while it is held, as when a frame is shown a second time. The pixels are the
// ones the slot already keeps, so the frame joins the acquisition instead of starting one.
TEST_CASE(surface_pool_holds_a_held_surface_again_when_it_is_handed_back)
{
    Atomic<u32> live_contents { 0 };
    auto pool = MUST(Media::VideoFrameSurfacePool::create());
    auto surface = make_surface();

    auto first = pool->try_acquire(surface, make_contents(live_contents)).value();
    auto again = pool->try_acquire(surface, make_contents(live_contents)).value();
    EXPECT_EQ(again.index, first.index);
    EXPECT_EQ(again.slot_acquisition_id, first.slot_acquisition_id);
    EXPECT_EQ(live_contents.load(), 1u);

    pool->release_hold(first.index);
    EXPECT_EQ(live_contents.load(), 1u);
    pool->release_hold(first.index);
    EXPECT_EQ(live_contents.load(), 0u);
}

TEST_CASE(shed_storage_releases_surfaces_as_their_holds_run_out)
{
    Atomic<u32> live_contents { 0 };
    auto pool = MUST(Media::VideoFrameSurfacePool::create());
    auto held_surface = make_surface();
    auto free_surface = make_surface();

    auto held = pool->try_acquire(held_surface, make_contents(live_contents)).value();
    auto freed = pool->try_acquire(free_surface, make_contents(live_contents)).value();
    pool->release_hold(freed.index);

    pool->shed_storage();

    // Nothing but this test refers to a surface the pool has let go of, while the held one's pixels are still kept.
    EXPECT_EQ(free_surface->ref_count(), 1u);
    EXPECT_EQ(live_contents.load(), 1u);

    pool->release_hold(held.index);
    EXPECT_EQ(held_surface->ref_count(), 1u);
    EXPECT_EQ(live_contents.load(), 0u);

    // The identities went with them, so a surface the pool held through the shed is a stranger to it again.
    auto reacquired = pool->try_acquire(held_surface, make_contents(live_contents)).value();
    EXPECT_NE(reacquired.allocated_buffer_id, held.allocated_buffer_id);
}

TEST_CASE(surface_pool_gives_distinct_surfaces_distinct_slots)
{
    auto pool = MUST(Media::VideoFrameSurfacePool::create());

    auto first_surface = make_surface();
    auto second_surface = make_surface();
    EXPECT_NE(first_surface->id(), second_surface->id());

    auto first = pool->try_acquire(first_surface, make_contents()).value();
    auto second = pool->try_acquire(second_surface, make_contents()).value();
    EXPECT_NE(second.index, first.index);
}

TEST_CASE(surface_pool_refuses_to_exceed_its_held_slot_count)
{
    auto pool = MUST(Media::VideoFrameSurfacePool::create());

    // The platform allocator hands out surfaces without limit, so this pool's held slot count is what stops a decoder
    // from running arbitrarily far ahead of whoever is displaying its frames.
    Vector<NonnullRefPtr<Media::VideoSurface>> surfaces;
    for (u32 index = 0; index < Media::VideoFrameSurfacePool::MAX_HELD_SLOT_COUNT; index++) {
        surfaces.append(make_surface());
        EXPECT(pool->try_acquire(surfaces.last(), make_contents()).has_value());
    }

    auto surface_too_many = make_surface();
    EXPECT(!pool->try_acquire(surface_too_many, make_contents()).has_value());

    // A held surface handed back again takes no further slot.
    EXPECT(pool->try_acquire(surfaces.first(), make_contents()).has_value());
    pool->release_hold(0);

    pool->release_hold(0);
    EXPECT(pool->try_acquire(surface_too_many, make_contents()).has_value());
}

TEST_CASE(surface_pool_remembers_more_surfaces_than_it_lets_be_held)
{
    auto pool = MUST(Media::VideoFrameSurfacePool::create());

    // Decoders cycle through more surfaces than may be held at once, and each one should be announced only once.
    static constexpr u32 decoder_surface_count = Media::VideoFrameSurfacePool::MAX_HELD_SLOT_COUNT * 2 + 1;
    static_assert(decoder_surface_count <= Media::VideoFrameSurfacePool::MAX_SLOT_COUNT);
    Vector<NonnullRefPtr<Media::VideoSurface>> surfaces;
    for (u32 index = 0; index < decoder_surface_count; index++)
        surfaces.append(make_surface());

    Vector<Media::VideoFrameSurfacePool::AcquiredSlot> first_cycle_acquisitions;
    Queue<u32> held_slot_indices;
    for (u32 cycle = 0; cycle < 3; cycle++) {
        for (u32 index = 0; index < decoder_surface_count; index++) {
            if (held_slot_indices.size() == Media::VideoFrameSurfacePool::MAX_HELD_SLOT_COUNT)
                pool->release_hold(held_slot_indices.dequeue());
            auto acquired = pool->try_acquire(surfaces[index], make_contents()).value();
            held_slot_indices.enqueue(acquired.index);
            if (cycle == 0) {
                first_cycle_acquisitions.append(acquired);
                continue;
            }
            EXPECT_EQ(acquired.index, first_cycle_acquisitions[index].index);
            EXPECT_EQ(acquired.allocated_buffer_id, first_cycle_acquisitions[index].allocated_buffer_id);
        }
    }
}

TEST_CASE(surface_pool_evicts_the_least_recently_acquired_identity_when_full)
{
    auto pool = MUST(Media::VideoFrameSurfacePool::create());

    Vector<NonnullRefPtr<Media::VideoSurface>> surfaces;
    Vector<Media::VideoFrameSurfacePool::AcquiredSlot> acquisitions;
    for (u32 index = 0; index < Media::VideoFrameSurfacePool::MAX_SLOT_COUNT; index++) {
        surfaces.append(make_surface());
        acquisitions.append(pool->try_acquire(surfaces.last(), make_contents()).value());
        pool->release_hold(acquisitions.last().index);
    }

    // Every slot remembers a surface, so a surface the pool has never seen has to displace one of them.
    auto new_surface = make_surface();
    auto acquired = pool->try_acquire(new_surface, make_contents()).value();
    EXPECT_EQ(acquired.index, acquisitions.first().index);
    EXPECT_NE(acquired.allocated_buffer_id, acquisitions.first().allocated_buffer_id);
    pool->release_hold(acquired.index);

    // The surface that was displaced is a stranger again, while the ones still on their slots are not.
    auto reacquired_evicted = pool->try_acquire(surfaces.first(), make_contents()).value();
    EXPECT_NE(reacquired_evicted.allocated_buffer_id, acquisitions.first().allocated_buffer_id);
    pool->release_hold(reacquired_evicted.index);

    auto reacquired_kept = pool->try_acquire(surfaces.last(), make_contents()).value();
    EXPECT_EQ(reacquired_kept.index, acquisitions.last().index);
    EXPECT_EQ(reacquired_kept.allocated_buffer_id, acquisitions.last().allocated_buffer_id);
}

#    ifdef AK_OS_MACOS
TEST_CASE(surface_slots_survive_a_mach_port_round_trip)
{
    auto pool = MUST(Media::VideoFrameSurfacePool::create());
    auto surface = make_surface();
    auto acquired = pool->try_acquire(surface, make_contents()).value();

    auto lent_surface = pool->slot_surface(acquired.index);
    EXPECT_NE(lent_surface, nullptr);

    auto reimported = MUST(Core::IOSurfaceHandle::from_mach_port(lent_surface->create_mach_port()));
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

    auto frame = MUST(Media::create_received_video_frame(metadata, {}, surface, 0, [] { }));
    EXPECT_EQ(frame->surface()->id(), surface->id());
    // The planes of a surface-backed frame are in the surface, so no view into a buffer is offered.
    EXPECT(!frame->yuv_data().has_value());

    auto larger_than_the_surface = metadata;
    larger_than_the_surface.size = { 32, 16 };
    EXPECT(Media::create_received_video_frame(larger_than_the_surface, {}, surface, 0, [] { }).is_error());

    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(4096));
    EXPECT(Media::create_received_video_frame(metadata, buffer, surface, 0, [] { }).is_error());
}

TEST_CASE(surface_slots_hold_and_free_through_the_shared_ledger)
{
    auto freed_count = 0u;
    auto pool = MUST(Media::VideoFrameSurfacePool::create());
    pool->set_slot_freed_callback([&] { freed_count++; });
    auto acquired = pool->try_acquire(make_surface(), make_contents()).value();

    pool->add_hold(acquired.index);
    pool->release_hold(acquired.index);
    EXPECT_EQ(freed_count, 0u);

    pool->release_hold(acquired.index);
    EXPECT_EQ(freed_count, 1u);
}

#endif
