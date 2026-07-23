/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

namespace Media {

// Linear PCM audio held entirely in memory: one Vector of samples per channel, all of equal length.
struct DecodedAudioData {
    Audio::SampleSpecification sample_specification;
    Vector<Vector<float>> channels;
};

MEDIA_API DecoderErrorOr<DecodedAudioData> decode_entire_audio_stream(NonnullRefPtr<MediaStream> const&, Optional<u32> output_sample_rate = {});

}
