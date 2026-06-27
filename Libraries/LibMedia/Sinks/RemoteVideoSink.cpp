/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <LibMedia/Sinks/RemoteVideoSink.h>
#include <LibMedia/VideoEdgeQueue.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoPresentation/PresentedFramePage.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

namespace Media {

class RemoteVideoSink::ThreadData : public AtomicRefCounted<RemoteVideoSink::ThreadData> {
public:
    ThreadData(VideoEdgeQueue, Delegates, PresentedFramePage);

    VideoEdgeQueue const& edge() const { return m_edge; }
    PresentedFramePage const& presented_frame_page() const { return m_presented_frame_page; }

    RefPtr<VideoFrame> current_frame();

    ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&);
    void disconnect_input(NonnullRefPtr<VideoProducer> const&);
    void transmit_seek() const;
    void transmit_time_reader(MediaTimeReader const&) const;
    void seek_upstream(AK::Duration timestamp);
    void start_input();
    void notify_space_available();
    void release_slot(VideoFramePoolID, u32 slot_index);

    void pump_thread_loop();
    void request_exit();
    void release_all_lends();

private:
    struct LentPool {
        NonnullRefPtr<VideoFramePool> pool;
        HashMap<u32, u32> lend_counts_by_slot_index;
        HashMap<u32, u64> announced_allocated_buffer_ids_by_slot_index;
        u64 outstanding_lend_count { 0 };
    };

    void enqueue_frame(VideoFrame const&, u32 seek_id);
    void lend_slot(PooledVideoFrameSlot const&);
    void wait_on_pump_thread(Sync::MutexLocker<Sync::Mutex>&);

    VideoEdgeQueue m_edge;
    Delegates m_delegates;

    mutable Sync::Mutex m_mutex;
    Sync::ConditionVariable m_wait_condition { m_mutex };
    RefPtr<VideoProducer> m_input;
    bool m_exit_requested { false };
    bool m_wake_pending { false };
    u32 m_actual_seek_id { 0 };

    Sync::Mutex m_lent_pools_mutex;
    HashMap<VideoFramePoolID, LentPool> m_lent_pools;
    PresentedFramePage m_presented_frame_page;
};

ErrorOr<NonnullRefPtr<RemoteVideoSink>> RemoteVideoSink::create(Delegates delegates)
{
    VERIFY(delegates.ring_data_available);
    VERIFY(delegates.transmit_seek);
    VERIFY(delegates.transmit_time_reader);
    VERIFY(delegates.announce_slot);
    VERIFY(delegates.retire_pool);
    auto edge = TRY(VideoEdgeQueue::create());
    auto presented_frame_page = TRY(PresentedFramePage::create());
    auto thread_data = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ThreadData(move(edge), move(delegates), move(presented_frame_page))));
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
PresentedFramePage const& RemoteVideoSink::presented_frame_page() const { return m_thread_data->presented_frame_page(); }
RefPtr<VideoFrame> RemoteVideoSink::current_frame() const { return m_thread_data->current_frame(); }

ErrorOr<void> RemoteVideoSink::connect_input(NonnullRefPtr<VideoProducer> const& producer) { return m_thread_data->connect_input(producer); }
void RemoteVideoSink::disconnect_input(NonnullRefPtr<VideoProducer> const& producer) { m_thread_data->disconnect_input(producer); }

void RemoteVideoSink::seek(AK::Duration) { m_thread_data->transmit_seek(); }
void RemoteVideoSink::seek_upstream(AK::Duration timestamp) { m_thread_data->seek_upstream(timestamp); }

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

RemoteVideoSink::ThreadData::ThreadData(VideoEdgeQueue edge, Delegates delegates, PresentedFramePage presented_frame_page)
    : m_edge(move(edge))
    , m_delegates(move(delegates))
    , m_presented_frame_page(move(presented_frame_page))
{
}

ErrorOr<void> RemoteVideoSink::ThreadData::connect_input(NonnullRefPtr<VideoProducer> const& producer)
{
    {
        Sync::MutexLocker locker { m_mutex };
        VERIFY(m_input == nullptr);
        m_input = producer;
    }
    producer->set_wake_handler([this] {
        Sync::MutexLocker locker { m_mutex };
        m_wake_pending = true;
        m_wait_condition.broadcast();
    });
    Sync::MutexLocker locker { m_mutex };
    m_wake_pending = true;
    m_wait_condition.broadcast();
    return {};
}

void RemoteVideoSink::ThreadData::disconnect_input(NonnullRefPtr<VideoProducer> const& producer)
{
    producer->set_wake_handler(nullptr);
    Sync::MutexLocker locker { m_mutex };
    VERIFY(m_input == producer.ptr());
    m_input = nullptr;
}

void RemoteVideoSink::ThreadData::transmit_seek() const
{
    m_delegates.transmit_seek();
}

void RemoteVideoSink::ThreadData::transmit_time_reader(MediaTimeReader const& time_reader) const
{
    m_delegates.transmit_time_reader(time_reader);
}

void RemoteVideoSink::ThreadData::seek_upstream(AK::Duration timestamp)
{
    Sync::MutexLocker locker { m_mutex };
    if (m_input)
        m_input->seek(timestamp);
    m_actual_seek_id++;
    m_wake_pending = true;
    m_wait_condition.broadcast();
}

void RemoteVideoSink::ThreadData::start_input()
{
    RefPtr<VideoProducer> input;
    {
        Sync::MutexLocker locker { m_mutex };
        input = m_input;
    }
    if (input)
        input->start();
}

void RemoteVideoSink::ThreadData::notify_space_available()
{
    Sync::MutexLocker locker { m_mutex };
    m_wake_pending = true;
    m_wait_condition.broadcast();
}

void RemoteVideoSink::ThreadData::request_exit()
{
    RefPtr<VideoProducer> input;
    {
        Sync::MutexLocker locker { m_mutex };
        m_exit_requested = true;
        input = move(m_input);
        m_wake_pending = true;
        m_wait_condition.broadcast();
    }
    if (input)
        input->set_wake_handler(nullptr);
}

void RemoteVideoSink::ThreadData::wait_on_pump_thread(Sync::MutexLocker<Sync::Mutex>&)
{
    while (!m_wake_pending && !m_exit_requested)
        m_wait_condition.wait();
    m_wake_pending = false;
}

void RemoteVideoSink::ThreadData::pump_thread_loop()
{
    Optional<u32> last_transmitted_seek_id;
    auto last_transmitted_status = PipelineStatus::Pending;

    while (true) {
        u32 seek_id;
        VideoProducerOutput output;
        RefPtr<VideoProducer> input;
        {
            Sync::MutexLocker locker { m_mutex };
            if (m_exit_requested)
                break;
            input = m_input;
            if (input == nullptr || !m_edge.can_enqueue()) {
                wait_on_pump_thread(locker);
                continue;
            }
            seek_id = m_actual_seek_id;
            output = input->peek();
        }

        if (output.status == PipelineStatus::HaveData) {
            VERIFY(output.frame);
            enqueue_frame(*output.frame, seek_id);
            input->consume();
            m_edge.set_status(PipelineStatus::Pending, seek_id);
            last_transmitted_seek_id = seek_id;
            last_transmitted_status = PipelineStatus::Pending;
            m_delegates.ring_data_available();
            continue;
        }

        if (last_transmitted_seek_id != seek_id || last_transmitted_status != output.status) {
            last_transmitted_seek_id = seek_id;
            last_transmitted_status = output.status;
            m_edge.set_status(output.status, seek_id);
            if (output.status == PipelineStatus::Suspended || is_terminal(output.status))
                m_delegates.ring_data_available();
        }
        Sync::MutexLocker locker { m_mutex };
        wait_on_pump_thread(locker);
    }

    release_all_lends();
}

void RemoteVideoSink::ThreadData::enqueue_frame(VideoFrame const& frame, u32 seek_id)
{
    auto const* pool_slot = frame.pool_slot();
    VERIFY(pool_slot != nullptr);
    lend_slot(*pool_slot);
    MUST(m_edge.enqueue(VideoFrameHandle::for_frame(frame), seek_id));
    m_edge.set_available_upper_bound(frame.conservative_end(), seek_id);
}

void RemoteVideoSink::ThreadData::lend_slot(PooledVideoFrameSlot const& pool_slot)
{
    auto& pool = pool_slot.pool();
    auto slot_index = pool_slot.slot_index();
    pool.add_hold(slot_index);
    Sync::MutexLocker locker { m_lent_pools_mutex };
    auto& lent_pool = m_lent_pools.ensure(pool.id(), [&] {
        return LentPool { .pool = pool, .lend_counts_by_slot_index = {}, .announced_allocated_buffer_ids_by_slot_index = {}, .outstanding_lend_count = 0 };
    });

    // Each new buffer backing the slot is announced exactly once, before the first handle that refers to it.
    auto announced_buffer_id = lent_pool.announced_allocated_buffer_ids_by_slot_index.get(slot_index);
    if (!announced_buffer_id.has_value() || *announced_buffer_id != pool_slot.allocated_buffer_id()) {
        m_delegates.announce_slot(pool.id(), slot_index, pool.slot_buffer(slot_index));
        lent_pool.announced_allocated_buffer_ids_by_slot_index.set(slot_index, pool_slot.allocated_buffer_id());
    }

    lent_pool.lend_counts_by_slot_index.ensure(slot_index, [] { return 0u; })++;
    lent_pool.outstanding_lend_count++;
}

void RemoteVideoSink::ThreadData::release_slot(VideoFramePoolID pool_id, u32 slot_index)
{
    RefPtr<VideoFramePool> pool;
    {
        Sync::MutexLocker locker { m_lent_pools_mutex };
        auto lent_pool = m_lent_pools.get(pool_id);
        if (!lent_pool.has_value())
            return;
        auto count = lent_pool->lend_counts_by_slot_index.get(slot_index);
        if (!count.has_value() || *count == 0)
            return;
        pool = lent_pool->pool;
        if (--*count == 0)
            lent_pool->lend_counts_by_slot_index.remove(slot_index);
        if (--lent_pool->outstanding_lend_count == 0) {
            m_lent_pools.remove(pool_id);
            m_delegates.retire_pool(pool_id);
        }
    }
    pool->release_hold(slot_index);
}

void RemoteVideoSink::ThreadData::release_all_lends()
{
    HashMap<VideoFramePoolID, LentPool> lent_pools;
    {
        Sync::MutexLocker locker { m_lent_pools_mutex };
        lent_pools = move(m_lent_pools);
    }
    for (auto& entry : lent_pools) {
        for (auto& [slot_index, lend_count] : entry.value.lend_counts_by_slot_index) {
            for (u32 release = 0; release < lend_count; release++)
                entry.value.pool->release_hold(slot_index);
        }
    }
}

RefPtr<VideoFrame> RemoteVideoSink::ThreadData::current_frame()
{
    auto handle = m_presented_frame_page.read();
    while (handle.has_value()) {
        {
            Sync::MutexLocker locker { m_lent_pools_mutex };
            auto lent_pool = m_lent_pools.get(handle->pool_id);
            if (lent_pool.has_value() && lent_pool->lend_counts_by_slot_index.contains(handle->slot_index)) {
                auto pool = lent_pool->pool;

                // Hold the slot across the synchronous read so it cannot be recycled mid-use; the resolved frame's
                // release callback drops the hold.
                pool->add_hold(handle->slot_index);
                auto frame_or_error = resolve_frame_from_slot_buffer(pool->slot_buffer(handle->slot_index), *handle, [pool, slot_index = handle->slot_index] {
                    pool->release_hold(slot_index);
                });
                if (frame_or_error.is_error()) {
                    pool->release_hold(handle->slot_index);
                    return nullptr;
                }
                return frame_or_error.release_value();
            }
        }

        // The handle's lend was already released, so a newer presented frame may have superseded it.
        auto updated_handle = m_presented_frame_page.read();
        auto handle_is_unchanged = updated_handle.has_value()
            && updated_handle->pool_id == handle->pool_id
            && updated_handle->slot_index == handle->slot_index
            && updated_handle->slot_acquisition_id == handle->slot_acquisition_id;
        if (handle_is_unchanged)
            return nullptr;
        handle = updated_handle;
    }
    return nullptr;
}

}
