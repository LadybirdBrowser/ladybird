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
#include <LibMedia/MediaTime.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/VideoFrameHandle.h>
#include <LibMedia/VideoFramePool.h>
#include <LibThreading/Forward.h>

namespace Media {

class PresentedFramePage;
class VideoEdgeQueue;

// The pump end of a cross-process video edge. Video frames are converted to handles and sent over a shared-memory
// queue across the boundary. When a frame with a new pool is sent, its pool is announced so that the other end can
// resolve it and any subsequent frames.
class MEDIA_API RemoteVideoSink final : public VideoSink {
public:
    struct Delegates {
        // Rung from the pump thread when newly enqueued data or a status change may need the consumer woken.
        Function<void()> ring_data_available;
        // A seek reaches the far-end consumer first so its cached frames are consulted before anything moves upstream.
        // The consumer will then seek upstream, which passes through the RemoteVideoProducer and across the edge.
        Function<void()> transmit_seek;
        // Relays a replacement time reader (e.g. after audio is disabled) to the consumer; the initial one rides video_edge_ready.
        Function<void(MediaTimeReader const&)> transmit_time_reader;
        // Called to lend each new buffer backing a slot to the other side, before the first handle that refers to it.
        Function<void(VideoFramePoolID, u32 slot_index, Core::AnonymousBuffer)> announce_slot;
        // Called when a pool's last lent slot is released, so the other side can drop its slot buffers.
        Function<void(VideoFramePoolID)> retire_pool;
    };

    static ErrorOr<NonnullRefPtr<RemoteVideoSink>> create(Delegates);
    virtual ~RemoteVideoSink() override;

    VideoEdgeQueue const& edge() const;
    PresentedFramePage const& presented_frame_page() const;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<VideoProducer> const&) override;

    virtual void seek(AK::Duration timestamp) override;
    void seek_upstream(AK::Duration timestamp);
    virtual void set_time_reader(MediaTimeReader) override;
    virtual void set_state_change_handler(PipelineStateChangeHandler) override;
    virtual void set_resize_handler(PipelineResizeHandler) override;
    virtual RefPtr<VideoFrame> current_frame() const override;
    void start_input();

    void report_remote_status(PipelineStatus);
    void report_remote_resize(Gfx::Size<u32>);

    // Callable from any thread.
    void notify_space_available();
    void release_slot(VideoFramePoolID, u32 slot_index);

private:
    class ThreadData;

    explicit RemoteVideoSink(NonnullRefPtr<ThreadData>);

    NonnullRefPtr<ThreadData> m_thread_data;

    PipelineStateChangeHandler m_on_state_changed;
    PipelineResizeHandler m_on_resize;
};

}
