/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <LibMedia/Sinks/RemoteVideoSink.h>
#include <LibMedia/VideoEdgeQueue.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoSurface.h>
#include <LibThreading/Thread.h>

namespace Media {

class RemoteVideoSink::ThreadData : public AtomicRefCounted<RemoteVideoSink::ThreadData> {
public:
    ThreadData(VideoEdgeQueue, Delegates);
    ~ThreadData();

    VideoEdgeQueue const& edge() const { return m_edge; }
    ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&);
    void disconnect_input(NonnullRefPtr<VideoProducer> const&);
    void ring_data_available() const;
    void transmit_seek(InvalidateHeldData) const;
    void transmit_time_reader(MediaTimeReader const&) const;
    void seek_upstream(AK::Duration timestamp, InvalidateHeldData);
    void seek_input_directly(AK::Duration timestamp);
    void start_input();
    void notify_space_available();
    void release_slot(VideoFramePoolID, u32 slot_index);

    void pump_thread_loop();
    void request_exit();
    void stop_and_reclaim_unconsumed_frames();
    bool has_lent_slots() const;
    void release_all_lends();

private:
    struct LentPool {
        NonnullRefPtr<VideoFrameEntryLedger> ledger;
        HashMap<u32, u32> lend_counts_by_slot_index;
        HashMap<u32, u64> announced_allocated_buffer_ids_by_slot_index;
        u64 outstanding_lend_count { 0 };
    };

    struct LentSlot {
        VideoFramePoolID pool_id;
        u32 slot_index { 0 };

        bool operator==(LentSlot const&) const = default;
    };
    struct LentSlotTraits : public DefaultTraits<LentSlot> {
        static unsigned hash(LentSlot const& slot) { return pair_int_hash(u64_hash(slot.pool_id.value()), slot.slot_index); }
    };
    struct LentFrameRange {
        AK::Duration timestamp;
        AK::Duration conservative_end;
    };

    void enqueue_frame(VideoFrame const&, u32 seek_id);
    void lend_slot(VideoFrame const&);
    bool consumer_may_satisfy_seek_locally(AK::Duration timestamp) const;
    void wait_on_pump_thread(MutexLocker<Mutex>&);

    VideoEdgeQueue m_edge;
    Delegates m_delegates;

    mutable Mutex m_mutex;
    ConditionVariable m_wait_condition { m_mutex };
    RefPtr<VideoProducer> m_input;
    bool m_exit_requested { false };
    bool m_pump_thread_exited { false };
    bool m_wake_pending { false };
    u32 m_actual_seek_id { 0 };
    // A seek the input was given directly, whose request from the consumer has yet to arrive.
    Optional<AK::Duration> m_direct_seek_timestamp;
    // The frame the pump has taken from the input and has yet to lend.
    Optional<LentFrameRange> m_peeked_frame_range;

    mutable Mutex m_lent_pools_mutex;
    HashMap<VideoFramePoolID, LentPool> m_lent_pools;
    HashMap<LentSlot, LentFrameRange, LentSlotTraits> m_lent_frame_ranges;
};

ErrorOr<NonnullRefPtr<RemoteVideoSink>> RemoteVideoSink::create(Delegates delegates)
{
    VERIFY(delegates.ring_data_available);
    VERIFY(delegates.transmit_seek);
    VERIFY(delegates.transmit_time_reader);
    VERIFY(delegates.announce_slot);
    VERIFY(delegates.retire_pool);
    auto edge = TRY(VideoEdgeQueue::create());
    auto thread_data = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ThreadData(move(edge), move(delegates))));
    auto sink = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) RemoteVideoSink(thread_data)));
    auto thread = Threading::Thread::construct("Video Pump"sv, [thread_data]() -> intptr_t {
        thread_data->pump_thread_loop();
        return 0;
    });
    thread->start();
    thread->detach();
    return sink;
}

RemoteVideoSink::RemoteVideoSink(NonnullRefPtr<ThreadData> thread_data)
    : m_thread_data(move(thread_data))
{
}

RemoteVideoSink::~RemoteVideoSink()
{
    m_thread_data->request_exit();
}

VideoEdgeQueue const& RemoteVideoSink::edge() const { return m_thread_data->edge(); }

ErrorOr<void> RemoteVideoSink::connect_input(NonnullRefPtr<VideoProducer> const& producer) { return m_thread_data->connect_input(producer); }
void RemoteVideoSink::disconnect_input(NonnullRefPtr<VideoProducer> const& producer) { m_thread_data->disconnect_input(producer); }

void RemoteVideoSink::seek(AK::Duration timestamp, InvalidateHeldData invalidate_held_data)
{
    if (invalidate_held_data == InvalidateHeldData::No)
        m_thread_data->seek_input_directly(timestamp);
    m_thread_data->transmit_seek(invalidate_held_data);
}
void RemoteVideoSink::notify_clock_resumed() { m_thread_data->ring_data_available(); }
void RemoteVideoSink::seek_upstream(AK::Duration timestamp, InvalidateHeldData invalidate_held_data) { m_thread_data->seek_upstream(timestamp, invalidate_held_data); }

void RemoteVideoSink::set_time_reader(MediaTimeReader time_reader)
{
    m_thread_data->transmit_time_reader(time_reader);
}

void RemoteVideoSink::set_state_change_handler(PipelineStateChangeHandler on_state_changed)
{
    m_on_state_changed = move(on_state_changed);
}

void RemoteVideoSink::set_resize_handler(PipelineResizeHandler on_resize)
{
    m_on_resize = move(on_resize);
}

void RemoteVideoSink::report_remote_status(PipelineStatus status)
{
    if (m_on_state_changed)
        m_on_state_changed(status);
}

void RemoteVideoSink::report_remote_resize(Gfx::Size<u32> size)
{
    if (m_on_resize)
        m_on_resize(size);
}

void RemoteVideoSink::start_input() { m_thread_data->start_input(); }
void RemoteVideoSink::notify_space_available() { m_thread_data->notify_space_available(); }
void RemoteVideoSink::release_slot(VideoFramePoolID pool_id, u32 slot_index) { m_thread_data->release_slot(pool_id, slot_index); }
void RemoteVideoSink::stop_and_reclaim_unconsumed_frames() { m_thread_data->stop_and_reclaim_unconsumed_frames(); }
bool RemoteVideoSink::has_lent_slots() const { return m_thread_data->has_lent_slots(); }

RemoteVideoSink::ThreadData::ThreadData(VideoEdgeQueue edge, Delegates delegates)
    : m_edge(move(edge))
    , m_delegates(move(delegates))
{
}

RemoteVideoSink::ThreadData::~ThreadData()
{
    release_all_lends();
}

ErrorOr<void> RemoteVideoSink::ThreadData::connect_input(NonnullRefPtr<VideoProducer> const& producer)
{
    producer->set_wake_handler([this] {
        MutexLocker locker { m_mutex };
        m_wake_pending = true;
        m_wait_condition.broadcast();
    });
    MutexLocker locker { m_mutex };
    VERIFY(m_input == nullptr);
    m_input = producer;
    m_wake_pending = true;
    m_wait_condition.broadcast();
    return {};
}

void RemoteVideoSink::ThreadData::disconnect_input(NonnullRefPtr<VideoProducer> const& producer)
{
    producer->set_wake_handler(nullptr);
    MutexLocker locker { m_mutex };
    VERIFY(m_input == producer.ptr());
    m_input = nullptr;
    m_direct_seek_timestamp = {};
}

void RemoteVideoSink::ThreadData::ring_data_available() const
{
    m_delegates.ring_data_available();
}

void RemoteVideoSink::ThreadData::transmit_seek(InvalidateHeldData invalidate_held_data) const
{
    m_delegates.transmit_seek(invalidate_held_data);
}

void RemoteVideoSink::ThreadData::transmit_time_reader(MediaTimeReader const& time_reader) const
{
    m_delegates.transmit_time_reader(time_reader);
}

void RemoteVideoSink::ThreadData::seek_upstream(AK::Duration timestamp, InvalidateHeldData invalidate_held_data)
{
    MutexLocker locker { m_mutex };
    auto direct_seek_timestamp = m_direct_seek_timestamp;
    m_direct_seek_timestamp = {};
    auto seek_was_given_directly = direct_seek_timestamp == timestamp && invalidate_held_data == InvalidateHeldData::No;
    if (m_input && !seek_was_given_directly)
        m_input->seek(timestamp, invalidate_held_data);
    m_actual_seek_id++;
    m_wake_pending = true;
    m_wait_condition.broadcast();
}

bool RemoteVideoSink::ThreadData::consumer_may_satisfy_seek_locally(AK::Duration timestamp) const
{
    // Everything the consumer could resolve the seek from is lent across the edge: the frames it shows and the ring,
    // and the frame on its way into the ring.
    Optional<AK::Duration> earliest_timestamp;
    Optional<AK::Duration> latest_end;
    auto include_range = [&](LentFrameRange const& range) {
        if (!earliest_timestamp.has_value() || range.timestamp < *earliest_timestamp)
            earliest_timestamp = range.timestamp;
        if (!latest_end.has_value() || range.conservative_end > *latest_end)
            latest_end = range.conservative_end;
    };
    {
        MutexLocker locker { m_lent_pools_mutex };
        for (auto const& [slot, range] : m_lent_frame_ranges)
            include_range(range);
    }
    if (m_peeked_frame_range.has_value())
        include_range(*m_peeked_frame_range);
    if (!earliest_timestamp.has_value() || timestamp < *earliest_timestamp)
        return false;
    if (timestamp < *latest_end)
        return true;
    auto status = m_edge.status();
    return status.has_value() && status->seek_id == m_actual_seek_id && status->status == PipelineStatus::EndOfStream;
}

void RemoteVideoSink::ThreadData::seek_input_directly(AK::Duration timestamp)
{
    MutexLocker locker { m_mutex };
    if (m_input == nullptr || m_direct_seek_timestamp.has_value() || consumer_may_satisfy_seek_locally(timestamp))
        return;
    // A request already on its way will seek the input to its own target, undoing this one.
    if (m_edge.requested_seek_id() != m_actual_seek_id)
        return;
    // Pumping stops until the consumer's request for this seek arrives, so that no frame of the new position is
    // stamped with the seek before it.
    m_direct_seek_timestamp = timestamp;
    m_input->seek(timestamp, InvalidateHeldData::No);
}

void RemoteVideoSink::ThreadData::start_input()
{
    RefPtr<VideoProducer> input;
    {
        MutexLocker locker { m_mutex };
        input = m_input;
    }
    if (input)
        input->start();
}

void RemoteVideoSink::ThreadData::notify_space_available()
{
    MutexLocker locker { m_mutex };
    m_wake_pending = true;
    m_wait_condition.broadcast();
}

void RemoteVideoSink::ThreadData::request_exit()
{
    RefPtr<VideoProducer> input;
    {
        MutexLocker locker { m_mutex };
        m_exit_requested = true;
        input = move(m_input);
        m_wake_pending = true;
        m_wait_condition.broadcast();
    }
    if (input)
        input->set_wake_handler(nullptr);
}

void RemoteVideoSink::ThreadData::stop_and_reclaim_unconsumed_frames()
{
    request_exit();
    {
        MutexLocker locker { m_mutex };
        while (!m_pump_thread_exited)
            m_wait_condition.wait();
    }

    for (auto item = m_edge.peek(); item.has_value(); item = m_edge.peek()) {
        release_slot(item->handle.pool_id, item->handle.slot_index);
        m_edge.consume();
    }
}

bool RemoteVideoSink::ThreadData::has_lent_slots() const
{
    MutexLocker locker { m_lent_pools_mutex };
    return !m_lent_pools.is_empty();
}

void RemoteVideoSink::ThreadData::wait_on_pump_thread(MutexLocker<Mutex>&)
{
    while (!m_wake_pending && !m_exit_requested)
        m_wait_condition.wait();
    m_wake_pending = false;
}

void RemoteVideoSink::ThreadData::pump_thread_loop()
{
    Optional<VideoEdgeStatus> published_status;
    auto publish_status = [&](VideoEdgeStatus status) {
        if (published_status == status)
            return false;
        published_status = status;
        m_edge.set_status(status.status, status.seek_id);
        return true;
    };

    // The seek ID a frame was enqueued under, while it has yet to be consumed from the input.
    Optional<u32> enqueued_frame_seek_id;

    while (true) {
        if (enqueued_frame_seek_id.has_value()) {
            {
                MutexLocker locker { m_mutex };
                m_peeked_frame_range = {};
                if (m_exit_requested)
                    break;
                // Paired with the fence in RemoteVideoProducer::seek(): either that seek's look at the ring saw this
                // frame, or its request is visible here and the frame stays in the input for the seek to find.
                AK::atomic_thread_fence(AK::MemoryOrder::memory_order_seq_cst);
                if (m_actual_seek_id != *enqueued_frame_seek_id) {
                    // A seek was applied since the peek, so the input's head is re-peeked; if unchanged, it is
                    // re-pumped under the new stamp and the consumer drops the old copy.
                    enqueued_frame_seek_id = {};
                    continue;
                }
                if (m_edge.requested_seek_id() != m_actual_seek_id || m_direct_seek_timestamp.has_value()) {
                    wait_on_pump_thread(locker);
                    continue;
                }
                if (m_input)
                    m_input->consume();
            }
            publish_status({ PipelineStatus::Pending, *enqueued_frame_seek_id });
            enqueued_frame_seek_id = {};
            m_delegates.ring_data_available();
            continue;
        }

        u32 seek_id;
        VideoProducerOutput output;
        {
            MutexLocker locker { m_mutex };
            if (m_exit_requested)
                break;
            auto seek_request_is_pending = m_edge.requested_seek_id() != m_actual_seek_id || m_direct_seek_timestamp.has_value();
            if (m_input == nullptr || !m_edge.can_enqueue() || seek_request_is_pending) {
                wait_on_pump_thread(locker);
                continue;
            }
            seek_id = m_actual_seek_id;
            output = m_input->peek();
            if (output.status == PipelineStatus::HaveData)
                m_peeked_frame_range = LentFrameRange { output.frame->timestamp(), output.frame->conservative_end() };
        }

        if (output.status == PipelineStatus::HaveData) {
            VERIFY(output.frame);
            enqueue_frame(*output.frame, seek_id);
            enqueued_frame_seek_id = seek_id;
            continue;
        }

        if (publish_status({ output.status, seek_id }) && (output.status == PipelineStatus::Suspended || is_terminal(output.status)))
            m_delegates.ring_data_available();
        MutexLocker locker { m_mutex };
        wait_on_pump_thread(locker);
    }

    MutexLocker locker { m_mutex };
    m_pump_thread_exited = true;
    m_wait_condition.broadcast();
}

void RemoteVideoSink::ThreadData::enqueue_frame(VideoFrame const& frame, u32 seek_id)
{
    lend_slot(frame);
    MUST(m_edge.enqueue(VideoFrameHandle::for_frame(frame), seek_id));
}

void RemoteVideoSink::ThreadData::lend_slot(VideoFrame const& frame)
{
    auto const* pool_slot = frame.pool_slot();
    VERIFY(pool_slot != nullptr);
    auto& ledger = pool_slot->ledger();
    auto slot_index = pool_slot->slot_index();
    ledger.add_hold(slot_index);
    MutexLocker locker { m_lent_pools_mutex };
    m_lent_frame_ranges.set({ ledger.id(), slot_index }, { frame.timestamp(), frame.conservative_end() });
    auto& lent_pool = m_lent_pools.ensure(ledger.id(), [&] {
        return LentPool { .ledger = ledger, .lend_counts_by_slot_index = {}, .announced_allocated_buffer_ids_by_slot_index = {}, .outstanding_lend_count = 0 };
    });

    // Each new buffer backing the slot is announced exactly once, before the first handle that refers to it.
    auto announced_buffer_id = lent_pool.announced_allocated_buffer_ids_by_slot_index.get(slot_index);
    if (!announced_buffer_id.has_value() || *announced_buffer_id != pool_slot->allocated_buffer_id()) {
        m_delegates.announce_slot(ledger.id(), slot_index, ledger.slot_buffer(slot_index), ledger.slot_surface(slot_index));
        lent_pool.announced_allocated_buffer_ids_by_slot_index.set(slot_index, pool_slot->allocated_buffer_id());
    }

    lent_pool.lend_counts_by_slot_index.ensure(slot_index, [] { return 0u; })++;
    lent_pool.outstanding_lend_count++;
}

void RemoteVideoSink::ThreadData::release_slot(VideoFramePoolID pool_id, u32 slot_index)
{
    RefPtr<VideoFrameEntryLedger> ledger;
    {
        MutexLocker locker { m_lent_pools_mutex };
        auto lent_pool = m_lent_pools.get(pool_id);
        if (!lent_pool.has_value())
            return;
        auto count = lent_pool->lend_counts_by_slot_index.get(slot_index);
        if (!count.has_value() || *count == 0)
            return;
        ledger = lent_pool->ledger;
        if (--*count == 0) {
            lent_pool->lend_counts_by_slot_index.remove(slot_index);
            m_lent_frame_ranges.remove({ pool_id, slot_index });
        }
        if (--lent_pool->outstanding_lend_count == 0) {
            m_lent_pools.remove(pool_id);
            m_delegates.retire_pool(pool_id);
        }
    }
    ledger->release_hold(slot_index);
}

void RemoteVideoSink::ThreadData::release_all_lends()
{
    HashMap<VideoFramePoolID, LentPool> lent_pools;
    {
        MutexLocker locker { m_lent_pools_mutex };
        lent_pools = move(m_lent_pools);
        m_lent_frame_ranges.clear();
    }
    for (auto& entry : lent_pools) {
        for (auto& [slot_index, lend_count] : entry.value.lend_counts_by_slot_index) {
            for (u32 release = 0; release < lend_count; release++)
                entry.value.ledger->release_hold(slot_index);
        }
    }
}

}
