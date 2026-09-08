/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Producers/RemoteVideoProducer.h>
#include <LibMedia/VideoFrame.h>

namespace Media {

NonnullRefPtr<RemoteVideoProducer> RemoteVideoProducer::create(VideoEdgeQueue edge, NonnullRefPtr<VideoFrameSlotDirectory> slot_directory, Delegates delegates)
{
    VERIFY(delegates.request_start);
    VERIFY(delegates.request_seek);
    VERIFY(delegates.release_slot);
    VERIFY(delegates.notify_space_available);
    auto producer = adopt_ref(*new RemoteVideoProducer(move(edge), move(slot_directory), move(delegates)));
    // A queued frame may have failed to resolve before its announcement arrived, so retry promptly.
    producer->m_slot_directory->set_on_slots_changed([weak = producer->make_weak_ref()] {
        if (auto strong = weak.strong_ref()) {
            if (strong->m_wake_handler)
                strong->m_wake_handler();
        }
    });
    return producer;
}

RemoteVideoProducer::RemoteVideoProducer(VideoEdgeQueue edge, NonnullRefPtr<VideoFrameSlotDirectory> slot_directory, Delegates delegates)
    : m_edge(move(edge))
    , m_slot_directory(move(slot_directory))
    , m_delegates(move(delegates))
{
}

void RemoteVideoProducer::notify_data_available()
{
    release_ring_contents_if_suspended();
    if (m_wake_handler)
        m_wake_handler();
}

void RemoteVideoProducer::release_ring_contents_if_suspended()
{
    auto status = m_edge.status();
    if (!status.has_value() || status->seek_id != m_expected_seek_id || status->status != PipelineStatus::Suspended)
        return;

    release_all_ring_frames();
}

void RemoteVideoProducer::discard_ring_head(VideoEdgeItem const& head)
{
    // A head already resolved into the current frame releases its lend through that frame instead.
    if (m_current_frame)
        m_current_frame = nullptr;
    else
        release_lent_slot(head.handle.pool_id, head.handle.slot_index);
    m_edge.consume();
}

void RemoteVideoProducer::discard_stale_ring_heads()
{
    auto discarded_any_frames = false;
    while (true) {
        auto head = m_edge.peek();
        if (!head.has_value() || head->seek_id == m_expected_seek_id)
            break;
        discard_ring_head(*head);
        discarded_any_frames = true;
    }
    if (discarded_any_frames)
        m_delegates.notify_space_available();
}

void RemoteVideoProducer::release_all_ring_frames()
{
    auto released_any_frames = false;
    while (true) {
        auto head = m_edge.peek();
        if (!head.has_value())
            break;
        discard_ring_head(*head);
        released_any_frames = true;
    }
    if (released_any_frames)
        m_delegates.notify_space_available();
}

void RemoteVideoProducer::start()
{
    m_delegates.request_start();
}

void RemoteVideoProducer::set_wake_handler(PipelineWakeHandler handler)
{
    m_wake_handler = move(handler);
}

void RemoteVideoProducer::seek(AK::Duration timestamp)
{
    discard_stale_ring_heads();

    // Announced before the ring is consulted, paired with the fence in the pump before it consumes an enqueued frame
    // from its input: either the look below sees the frame, or the pump sees the request and leaves the frame in its
    // input for the seek to find. A locally satisfied seek withdraws the announcement; the pump is woken by the
    // consume that follows.
    auto requested_seek_id = m_expected_seek_id + 1;
    m_edge.set_requested_seek_id(requested_seek_id);
    AK::atomic_thread_fence(AK::MemoryOrder::memory_order_seq_cst);
    if (can_satisfy_seek_locally(timestamp)) {
        m_edge.set_requested_seek_id(m_expected_seek_id);
        return;
    }
    m_expected_seek_id = requested_seek_id;

    release_all_ring_frames();
    m_delegates.request_seek(timestamp);
}

bool RemoteVideoProducer::can_satisfy_seek_locally(AK::Duration timestamp) const
{
    // The lookahead is the ring itself, from its head to the conservative end of its newest frame. Only this side
    // moves the head, so the two reads describe one snapshot of the ring.
    auto head = m_edge.peek();
    if (!head.has_value() || head->seek_id != m_expected_seek_id || timestamp < head->handle.timestamp)
        return false;
    if (timestamp < m_edge.peek_newest()->handle.conservative_end())
        return true;
    auto status = m_edge.status();
    return status.has_value() && status->seek_id == m_expected_seek_id && status->status == PipelineStatus::EndOfStream;
}

VideoProducerOutput RemoteVideoProducer::peek()
{
    discard_stale_ring_heads();
    auto head = m_edge.peek();
    if (!head.has_value()) {
        auto status = m_edge.status();
        if (!status.has_value() || status->seek_id != m_expected_seek_id)
            return { nullptr, PipelineStatus::Pending };
        return { nullptr, status->status };
    }

    if (!m_current_frame)
        m_current_frame = resolve(head->handle);
    if (!m_current_frame)
        return { nullptr, PipelineStatus::Pending };
    return { m_current_frame, PipelineStatus::HaveData };
}

void RemoteVideoProducer::consume()
{
    m_current_frame = nullptr;
    m_edge.consume();
    m_delegates.notify_space_available();
}

void RemoteVideoProducer::release_lent_slot(VideoFramePoolID pool_id, u32 slot_index) const
{
    m_delegates.release_slot(pool_id, slot_index);
}

RefPtr<VideoFrame> RemoteVideoProducer::resolve(VideoFrameHandle const& handle)
{
    return m_slot_directory->resolve_frame(handle, [weak = make_weak_ref(), pool_id = handle.pool_id, slot_index = handle.slot_index] {
        if (auto producer = weak.strong_ref())
            producer->release_lent_slot(pool_id, slot_index);
    });
}

}
