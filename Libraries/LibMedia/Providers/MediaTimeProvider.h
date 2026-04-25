/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/MediaPipelineNode.h>

namespace Media {

class MediaTimeProvider : public virtual MediaPipelineNode {
public:
    virtual ~MediaTimeProvider() = default;

    virtual AK::Duration current_time() const = 0;
    virtual void resume() = 0;
    virtual void pause() = 0;
    virtual void set_time(AK::Duration) = 0;
};

}
