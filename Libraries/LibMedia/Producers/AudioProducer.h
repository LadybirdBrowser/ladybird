/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/MediaPipelineNode.h>

namespace Media {

class AudioProducer : public virtual MediaPipelineNode {
public:
    virtual ~AudioProducer() = default;
};

}
