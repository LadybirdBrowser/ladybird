/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Producers/AudioProducer.h>
#include <LibMedia/Sinks/AudioSink.h>

namespace Media {

class AudioProcessor : public AudioProducer
    , public AudioSink {
public:
    virtual ~AudioProcessor() = default;
};

}
