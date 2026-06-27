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

void RemoteVideoProducer::release_all_ring_frames()
{
    auto released_any_frames = false;
    if (m_current_frame) {
        m_current_frame.clear();
        m_edge.consume();
        released_any_frames = true;
    }
    while (true) {
        auto item = m_edge.peek();
        if (!item.has_value())
            break;
        release_lent_slot(item->handle.pool_id, item->handle.slot_index);
        m_edge.consume();
        released_any_frames = true;
    }
    if (released_any_frames && m_delegates.notify_space_available)
        m_delegates.notify_space_available();
}

void RemoteVideoProducer::start()
{
    if (m_delegates.request_start)
        m_delegates.request_start();
}

void RemoteVideoProducer::set_wake_handler(PipelineWakeHandler handler)
{
    m_wake_handler = move(handler);
}

void RemoteVideoProducer::seek(AK::Duration timestamp)
{
    if (can_satisfy_seek_locally(timestamp))
        return;
    m_expected_seek_id++;

    release_all_ring_frames();

    if (m_delegates.request_seek)
        m_delegates.request_seek(timestamp);
}

bool RemoteVideoProducer::can_satisfy_seek_locally(AK::Duration timestamp) const
{
    auto upper_bound = m_edge.available_upper_bound();
    if (!upper_bound.has_value() || upper_bound->seek_id != m_expected_seek_id)
        return false;
    if (timestamp > upper_bound->timestamp) {
        auto status = m_edge.status();
        if (!status.has_value() || status->seek_id != m_expected_seek_id || status->status != PipelineStatus::EndOfStream)
            return false;
    }

    if (m_current_frame)
        return timestamp >= m_current_frame->timestamp();
    auto head = m_edge.peek();
    return head.has_value() && head->seek_id == m_expected_seek_id && timestamp >= head->handle.timestamp;
}

VideoProducerOutput RemoteVideoProducer::peek()
{
    while (true) {
        auto item = m_edge.peek();
        if (!item.has_value()) {
            auto status = m_edge.status();
            if (!status.has_value() || status->seek_id != m_expected_seek_id)
                return { nullptr, PipelineStatus::Pending };
            return { nullptr, status->status };
        }

        if (item->seek_id != m_expected_seek_id) {
            // If we have a current frame here, we've resolved it in a previous peek() without consume()ing it, so it
            // will take care of releasing automatically.
            if (m_current_frame)
                m_current_frame = nullptr;
            else
                release_lent_slot(item->handle.pool_id, item->handle.slot_index);
            m_edge.consume();
            if (m_delegates.notify_space_available)
                m_delegates.notify_space_available();
            continue;
        }

        if (!m_current_frame)
            m_current_frame = resolve(item->handle);
        if (!m_current_frame)
            return { nullptr, PipelineStatus::Pending };
        return { m_current_frame, PipelineStatus::HaveData };
    }
}

void RemoteVideoProducer::consume()
{
    m_current_frame = nullptr;
    m_edge.consume();
    if (m_delegates.notify_space_available)
        m_delegates.notify_space_available();
}

void RemoteVideoProducer::release_lent_slot(VideoFramePoolID pool_id, u32 slot_index) const
{
    if (m_delegates.release_slot)
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
