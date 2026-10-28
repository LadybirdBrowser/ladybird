/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/ThreadSafeWeakable.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/SynchronizedWakeHandler.h>

namespace Media {

// The consumer end of a cross-process video edge.
class MEDIA_API RemoteVideoProducer final
    : public VideoProducer
    , public ThreadSafeWeakable<RemoteVideoProducer> {
public:
    // A frame arriving past this is released.
    static constexpr size_t MAX_QUEUED_FRAME_COUNT = 32;

    struct Delegates {
        Function<void()> request_start;
        Function<void(AK::Duration, InvalidateHeldData, u64 newest_received_lend_id)> request_seek;
    };

    static NonnullRefPtr<RemoteVideoProducer> create(Delegates);
    virtual ~RemoteVideoProducer() override;

    // Dropping the frame releases it back to the sink.
    void receive_frame(u64 lend_id, u32 seek_id, NonnullRefPtr<VideoFrame>);
    void receive_status(PipelineStatus, u32 seek_id);
    void notify_clock_resumed();

    void stop_consuming();

    virtual void start() override;
    virtual VideoProducerOutput peek() override;
    virtual void consume() override;
    virtual void set_wake_handler(PipelineWakeHandler) override;
    virtual void seek(AK::Duration timestamp, InvalidateHeldData = InvalidateHeldData::No) override;

private:
    explicit RemoteVideoProducer(Delegates);

    struct ReceivedStatus {
        PipelineStatus status { PipelineStatus::Pending };
        u32 seek_id { 0 };
    };

    bool can_satisfy_seek_locally(AK::Duration timestamp) const;
    bool has_end_of_stream_status() const;

    Delegates m_delegates;
    bool m_has_stopped_consuming { false };

    u32 m_expected_seek_id { 0 };
    u64 m_newest_received_lend_id { 0 };
    // Received under the expected seek, oldest first.
    Vector<NonnullRefPtr<VideoFrame>> m_queued_frames;
    Optional<ReceivedStatus> m_status;
    SynchronizedWakeHandler m_wake_handler;
};

}
