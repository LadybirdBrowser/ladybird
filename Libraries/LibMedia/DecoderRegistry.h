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
MEDIA_API DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_audio_decoder(CodecID, Audio::SampleSpecification const&, ReadonlyBytes codec_initialization_data);
MEDIA_API DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_video_decoder(CodecID, ReadonlyBytes codec_initialization_data);

}
