/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioDecoder.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoDecoder.h>

namespace Media {

MEDIA_API Optional<DecoderCapabilities> decoder_capabilities(ParsedCodec const&);

// Indicates which decoder was selected for a codec, so that the selection can be compared upon a config change before
// tearing down an existing decoder. Each kind of track indexes its own registry, so their selections are distinct
// types and cannot be exchanged.
template<TrackType track_type>
class DecoderSelection {
public:
    DecoderSelection() = default;
    explicit DecoderSelection(i32 registration_index)
        : m_registration_index(registration_index)
    {
        VERIFY(registration_index >= 0);
    }

    bool has_value() const { return m_registration_index >= 0; }
    i32 registration_index() const { return m_registration_index; }

    bool operator==(DecoderSelection const&) const = default;

private:
    i32 m_registration_index { -1 };
};

using AudioDecoderSelection = DecoderSelection<TrackType::Audio>;
using VideoDecoderSelection = DecoderSelection<TrackType::Video>;

// Identifies the highest-priority decoder after the optionally-provided selection that can handle this codec.
MEDIA_API AudioDecoderSelection select_audio_decoder(ParsedCodec const&, AudioDecoderSelection after = {});
MEDIA_API VideoDecoderSelection select_video_decoder(ParsedCodec const&, VideoDecoderSelection after = {});

MEDIA_API DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_audio_decoder(AudioDecoderSelection, CodecID, Audio::SampleSpecification const&, ReadonlyBytes codec_initialization_data);
MEDIA_API DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_video_decoder(VideoDecoderSelection, CodecID, ReadonlyBytes codec_initialization_data);

}
