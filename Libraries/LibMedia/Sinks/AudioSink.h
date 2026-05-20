/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaPipelineNode.h>
#include <LibMedia/Producers/AudioProducer.h>

namespace Media {

class MEDIA_API AudioSink : public virtual MediaPipelineNode {
public:
    virtual ~AudioSink() = default;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) = 0;
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) = 0;

    virtual void seek(AK::Duration timestamp) = 0;
};

}
