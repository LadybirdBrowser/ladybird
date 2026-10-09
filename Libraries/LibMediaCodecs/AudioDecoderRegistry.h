/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibAudio/SampleSpecification.h>
#include <LibMediaCodecs/AudioDecoder.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/DecoderSelection.h>
#include <LibMediaCodecs/Export.h>

namespace Media {

using AudioDecoderFactory = DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> (*)(CodecID, Audio::SampleSpecification const&, ReadonlyBytes);

struct AudioDecoderRegistration {
    DecoderCapabilitiesFunction capabilities;
    AudioDecoderFactory create;
};

// Each process installs the sets of audio decoders it hosts, in priority order, before it decodes anything.
MEDIACODECS_API void install_audio_decoders(Vector<ReadonlySpan<AudioDecoderRegistration>>);

MEDIACODECS_API Optional<DecoderCapabilities> audio_decoder_capabilities(ParsedCodec const&);

// Identifies the highest-priority decoder after the optionally-provided selection that can handle this codec.
MEDIACODECS_API AudioDecoderSelection select_audio_decoder(ParsedCodec const&, AudioDecoderSelection after = {});

MEDIACODECS_API DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_audio_decoder(AudioDecoderSelection, CodecID, Audio::SampleSpecification const&, ReadonlyBytes codec_initialization_data);

}
