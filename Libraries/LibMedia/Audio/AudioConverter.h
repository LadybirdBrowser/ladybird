/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>

namespace Audio {

class AudioConverter {
public:
    virtual ErrorOr<void> set_output_sample_specification(SampleSpecification) = 0;
    virtual ErrorOr<void> convert(Media::AudioBlock& input) = 0;

    virtual ~AudioConverter() = default;
};

}
