/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>

namespace Media {

struct MEDIA_API DecodedAudioData {
    Audio::SampleSpecification sample_specification;
    u32 frame_count() const { return sample_specification.channel_count() == 0 ? 0 : interleaved_f32_samples.size() / sample_specification.channel_count(); }
    Vector<f32> interleaved_f32_samples;
};

MEDIA_API DecoderErrorOr<DecodedAudioData> decode_first_audio_track_to_pcm_f32(ByteBuffer&& encoded_data, Optional<u32> target_sample_rate = {});

MEDIA_API DecoderErrorOr<DecodedAudioData> decode_first_audio_track_to_pcm_f32(ByteBuffer&& encoded_data, Optional<u32> target_sample_rate, Function<bool()> is_cancelled);

}
