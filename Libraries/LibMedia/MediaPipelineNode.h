/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>

namespace Media {

class MediaPipelineNode : public AtomicRefCounted<MediaPipelineNode> {
public:
    virtual ~MediaPipelineNode() = default;
};

}
