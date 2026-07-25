/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecoderError.h>

namespace Audio {

class AudioConverter {
public:
    virtual ErrorOr<void> set_output_sample_specification(SampleSpecification) = 0;

    virtual ErrorOr<void> push_block(Media::AudioBlock const&) = 0;
    virtual Media::DecoderErrorOr<void> retrieve_block(Media::AudioBlock& into) = 0;
    virtual void signal_end_of_stream() = 0;
    virtual void flush() = 0;

    virtual ~AudioConverter() = default;
};

}
