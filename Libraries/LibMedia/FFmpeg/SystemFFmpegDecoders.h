/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibMedia/AudioDecoder.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/VideoDecoder.h>

namespace Media::FFmpeg {

Optional<DecoderCapabilities> system_ffmpeg_video_decoder_capabilities(ParsedCodec const&);
Optional<DecoderCapabilities> system_ffmpeg_audio_decoder_capabilities(ParsedCodec const&);
DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_system_ffmpeg_video_decoder(CodecID, ReadonlyBytes codec_initialization_data);
DecoderErrorOr<NonnullOwnPtr<AudioDecoder>> create_system_ffmpeg_audio_decoder(CodecID, Audio::SampleSpecification const&, ReadonlyBytes codec_initialization_data);

}
