/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Forward.h>
#include <LibMedia/MediaPipelineNode.h>
#include <LibMedia/PipelineStatus.h>

namespace Media {

class VideoProducer : public virtual MediaPipelineNode {
public:
    virtual ~VideoProducer() = default;

    virtual PipelineStatus pull(RefPtr<VideoFrame>& into) = 0;
    virtual void set_state_changed_handler(PipelineStateChangeHandler) = 0;
};

}
