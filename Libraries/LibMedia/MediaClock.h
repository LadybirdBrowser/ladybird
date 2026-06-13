/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/MediaPipelineNode.h>
#include <LibMedia/MediaTime.h>

namespace Media {

class MediaClock : public virtual MediaPipelineNode {
public:
    virtual ~MediaClock() = default;

    virtual MediaTimeReader time_reader() const = 0;
    virtual void resume() = 0;
    virtual void pause() = 0;
    virtual void seek(AK::Duration) = 0;
    virtual void set_playback_rate(float) = 0;
};

}
