/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/MediaPipelineNode.h>
#include <LibMedia/PipelineStatus.h>

namespace Media {

class AudioProducer : public virtual MediaPipelineNode {
public:
    virtual ~AudioProducer() = default;

    virtual PipelineStatus pull(AudioBlock& into) = 0;
    virtual void set_state_changed_handler(PipelineStateChangeHandler) = 0;

    virtual void seek(AK::Duration timestamp) = 0;
};

}
