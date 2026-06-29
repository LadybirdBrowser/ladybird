/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaPipelineNode.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/SeekMode.h>

namespace Media {

struct VideoProducerOutput {
    RefPtr<VideoFrame> frame;
    PipelineStatus status;
};

class VideoProducer : public virtual MediaPipelineNode {
public:
    virtual ~VideoProducer() = default;

    virtual void start() = 0;

    virtual VideoProducerOutput peek() = 0;
    virtual void consume() = 0;
    virtual void set_wake_handler(PipelineWakeHandler) = 0;

    virtual void seek(AK::Duration timestamp) = 0;
};

}
