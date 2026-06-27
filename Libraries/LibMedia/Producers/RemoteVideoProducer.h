/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/VideoEdgeQueue.h>
#include <LibMedia/VideoFrameHandle.h>
#include <LibMedia/VideoFramePool.h>
#include <LibSync/Weakable.h>

namespace Media {

// The consumer end of a cross-process video edge: reads frame handles through a shared memory VideoEdgeQueue, resolves
// them through the edge's VideoFrameSlotDirectory, and presents them through the VideoProducer interface, so a
// DisplayingVideoSink consumes it exactly as it would a local producer. Communication crosses the process boundary
// through delegates that relay messages over IPC.
class MEDIA_API RemoteVideoProducer final
    : public VideoProducer
    , public Sync::Weakable<RemoteVideoProducer> {
public:
    struct Delegates {
        Function<void()> request_start;
        Function<void(AK::Duration)> request_seek;
        Function<void(VideoFramePoolID, u32 slot_index)> release_slot;
        Function<void()> notify_space_available;
    };

    static NonnullRefPtr<RemoteVideoProducer> create(VideoEdgeQueue, NonnullRefPtr<VideoFrameSlotDirectory>, Delegates);

    void notify_data_available();

    virtual void start() override;
    virtual VideoProducerOutput peek() override;
    virtual void consume() override;
    virtual void set_wake_handler(PipelineWakeHandler) override;
    virtual void seek(AK::Duration timestamp) override;

private:
    RemoteVideoProducer(VideoEdgeQueue, NonnullRefPtr<VideoFrameSlotDirectory>, Delegates);

    RefPtr<VideoFrame> resolve(VideoFrameHandle const&);
    void release_lent_slot(VideoFramePoolID, u32 slot_index) const;
    bool can_satisfy_seek_locally(AK::Duration timestamp) const;
    void release_ring_contents_if_suspended();
    void release_all_ring_frames();

    VideoEdgeQueue m_edge;
    NonnullRefPtr<VideoFrameSlotDirectory> m_slot_directory;
    Delegates m_delegates;

    u32 m_expected_seek_id { 0 };
    RefPtr<VideoFrame> m_current_frame;
    PipelineWakeHandler m_wake_handler;
};

}
