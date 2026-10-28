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
#include <LibCore/AnonymousBuffer.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFrameMetadata.h>
#include <LibThreading/Forward.h>

namespace Media {

// The pump end of a cross-process video edge.
class MEDIA_API RemoteVideoSink final : public VideoSink {
public:
    static constexpr size_t MAX_LENT_FRAME_COUNT = 32;

    struct LentFrame {
        u64 lend_id { 0 };
        u32 seek_id { 0 };
        VideoFrameMetadata metadata;
        VideoFramePixels pixels;
    };

    struct Delegates {
        // Called from the pump thread, in the order the frames are lent.
        Function<void(LentFrame)> send_frame;
        // Called from the pump thread, after the frames the status follows.
        Function<void(PipelineStatus, u32 seek_id)> send_status;
        Function<void()> notify_clock_resumed;
        // A seek reaches the far-end consumer so that its cached frames are consulted. Unless the frames lent to it
        // could satisfy the seek, the input is seeked before the seek is transmitted, and the consumer's request for
        // the same seek, which passes through the RemoteVideoProducer and across the edge, is then already answered.
        Function<void(InvalidateHeldData)> transmit_seek;
        // Relays a replacement time reader (e.g. after audio is disabled) to the consumer; the initial one rides video_edge_ready.
        Function<void(MediaTimeReader const&)> transmit_time_reader;
    };

    static ErrorOr<NonnullRefPtr<RemoteVideoSink>> create(Delegates);
    virtual ~RemoteVideoSink() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<VideoProducer> const&) override;

    virtual void seek(AK::Duration timestamp, InvalidateHeldData = InvalidateHeldData::No) override;
    virtual void notify_clock_resumed() override;
    // The newest received lend ID is the last one the consumer had seen when it asked for the seek.
    void seek_upstream(AK::Duration timestamp, InvalidateHeldData, u64 newest_received_lend_id);
    virtual void set_time_reader(MediaTimeReader) override;
    virtual void set_state_change_handler(PipelineStateChangeHandler) override;
    virtual void set_resize_handler(PipelineResizeHandler) override;
    void start_input();

    void report_remote_status(PipelineStatus);
    void report_remote_resize(Gfx::Size<u32>);

    // Callable from any thread.
    void release_frame(u64 lend_id);

    // The frames already lent stay lent until they are released.
    void stop();
    bool has_lent_frames() const;

private:
    class ThreadData;

    explicit RemoteVideoSink(NonnullRefPtr<ThreadData>);

    NonnullRefPtr<ThreadData> m_thread_data;

    PipelineStateChangeHandler m_on_state_changed;
    PipelineResizeHandler m_on_resize;
};

}
