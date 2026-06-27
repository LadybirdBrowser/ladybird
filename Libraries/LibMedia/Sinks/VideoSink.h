/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaPipelineNode.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/VideoProducer.h>

namespace Media {

// A consumer to be attached to a VideoProducer in order to receive video frames from a decoding thread.
class VideoSink : public virtual MediaPipelineNode {
public:
    virtual ~VideoSink() = default;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&) = 0;
    virtual void disconnect_input(NonnullRefPtr<VideoProducer> const&) = 0;

    virtual void seek(AK::Duration timestamp) = 0;

    virtual void set_time_reader(MediaTimeReader) = 0;
    virtual void set_state_change_handler(PipelineStateChangeHandler) = 0;
    virtual void set_resize_handler(PipelineResizeHandler) = 0;

    virtual RefPtr<VideoFrame> current_frame() const = 0;
};

}
