/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Producers/RemoteVideoProducer.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Media {

NonnullRefPtr<RemoteVideoProducer> RemoteVideoProducer::create(Delegates delegates)
{
    VERIFY(delegates.request_start);
    VERIFY(delegates.request_seek);
    return adopt_ref(*new RemoteVideoProducer(move(delegates)));
}

RemoteVideoProducer::RemoteVideoProducer(Delegates delegates)
    : m_delegates(move(delegates))
{
}

RemoteVideoProducer::~RemoteVideoProducer() = default;

void RemoteVideoProducer::stop_consuming()
{
    m_queued_frames.clear();
    m_has_stopped_consuming = true;
}

void RemoteVideoProducer::receive_frame(u64 lend_id, u32 seek_id, NonnullRefPtr<VideoFrame> frame)
{
    m_newest_received_lend_id = max(m_newest_received_lend_id, lend_id);
    if (m_has_stopped_consuming || seek_id != m_expected_seek_id)
        return;
    if (m_queued_frames.size() >= MAX_QUEUED_FRAME_COUNT) {
        dbgln("RemoteVideoProducer: Releasing a frame received past the limit of {} queued frames", MAX_QUEUED_FRAME_COUNT);
        return;
    }
    m_queued_frames.append(move(frame));
    m_wake_handler.dispatch();
}

void RemoteVideoProducer::receive_status(PipelineStatus status, u32 seek_id)
{
    if (m_has_stopped_consuming)
        return;
    // A peer can send any value for the status.
    if (to_underlying(status) > to_underlying(PipelineStatus::Error))
        status = PipelineStatus::Error;
    m_status = ReceivedStatus { status, seek_id };

    // A suspended producer has let go of its decoder, and resuming seeks past the frames it lent anyway.
    if (status == PipelineStatus::Suspended && seek_id == m_expected_seek_id)
        m_queued_frames.clear();
    m_wake_handler.dispatch();
}

void RemoteVideoProducer::notify_clock_resumed()
{
    if (m_has_stopped_consuming)
        return;
    m_wake_handler.dispatch();
}

void RemoteVideoProducer::start()
{
    m_delegates.request_start();
}

void RemoteVideoProducer::set_wake_handler(PipelineWakeHandler handler)
{
    m_wake_handler.set(move(handler));
}

void RemoteVideoProducer::seek(AK::Duration timestamp, InvalidateHeldData invalidate_held_data)
{
    if (m_has_stopped_consuming)
        return;
    if (invalidate_held_data == InvalidateHeldData::No && can_satisfy_seek_locally(timestamp))
        return;

    // Released only after the request, so that the sink hears of the seek before their storage is decoded into again.
    auto stale_frames = move(m_queued_frames);
    m_expected_seek_id++;
    m_delegates.request_seek(timestamp, invalidate_held_data, m_newest_received_lend_id);
    stale_frames.clear();
}

bool RemoteVideoProducer::has_end_of_stream_status() const
{
    return m_status.has_value() && m_status->seek_id == m_expected_seek_id && m_status->status == PipelineStatus::EndOfStream;
}

bool RemoteVideoProducer::can_satisfy_seek_locally(AK::Duration timestamp) const
{
    if (m_queued_frames.is_empty() || timestamp < m_queued_frames.first()->timestamp())
        return false;
    if (timestamp < m_queued_frames.last()->conservative_end())
        return true;
    return has_end_of_stream_status();
}

VideoProducerOutput RemoteVideoProducer::peek()
{
    if (m_has_stopped_consuming)
        return { nullptr, PipelineStatus::Pending };
    if (!m_queued_frames.is_empty())
        return { m_queued_frames.first(), PipelineStatus::HaveData };
    if (!m_status.has_value() || m_status->seek_id != m_expected_seek_id)
        return { nullptr, PipelineStatus::Pending };
    return { nullptr, m_status->status };
}

void RemoteVideoProducer::consume()
{
    if (m_has_stopped_consuming || m_queued_frames.is_empty())
        return;
    m_queued_frames.remove(0);
}

}
