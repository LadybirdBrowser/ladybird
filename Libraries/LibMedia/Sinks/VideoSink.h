/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaPipelineNode.h>

namespace Media {

// A consumer to be attached to a DecodedVideoProducer in order to receive video frames from a decoding thread.
class VideoSink : public virtual MediaPipelineNode {
public:
    virtual ~VideoSink() = default;

    virtual void set_producer(Track const&, RefPtr<DecodedVideoProducer> const&) = 0;
    virtual RefPtr<DecodedVideoProducer> producer(Track const&) const = 0;

    virtual void seek(AK::Duration timestamp) = 0;
};

}
