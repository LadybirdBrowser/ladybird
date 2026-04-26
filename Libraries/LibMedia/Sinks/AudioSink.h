/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaPipelineNode.h>
#include <LibMedia/Track.h>

namespace Media {

class MEDIA_API AudioSink : public virtual MediaPipelineNode {
public:
    virtual ~AudioSink() = default;

    virtual void set_producer(Track const&, RefPtr<DecodedAudioProducer> const&) = 0;
    virtual RefPtr<DecodedAudioProducer> producer(Track const&) const = 0;

    virtual void seek(AK::Duration timestamp) = 0;
};

}
