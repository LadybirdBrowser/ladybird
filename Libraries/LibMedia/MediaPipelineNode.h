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
    // GCC false-positives -Wfree-nonheap-object when a virtually-derived subclass is deleted through a thunk.
    AK_IGNORE_DIAGNOSTIC("-Wfree-nonheap-object", virtual ~MediaPipelineNode() = default;)
};

}
