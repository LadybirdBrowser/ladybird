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
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/DecoderSelection.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/VideoDecoder.h>

namespace Media {

using VideoDecoderFactory = DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> (*)(CodecID, ReadonlyBytes);

struct VideoDecoderRegistration {
    DecoderCapabilitiesFunction capabilities;
    VideoDecoderFactory create;
};

// Each process installs the sets of video decoders it hosts, in priority order, before it decodes anything.
MEDIACODECS_API void install_video_decoders(Vector<ReadonlySpan<VideoDecoderRegistration>>);

MEDIACODECS_API Optional<DecoderCapabilities> video_decoder_capabilities(ParsedCodec const&);

// Identifies the highest-priority decoder after the optionally-provided selection that can handle this codec.
MEDIACODECS_API VideoDecoderSelection select_video_decoder(ParsedCodec const&, VideoDecoderSelection after = {});

MEDIACODECS_API DecoderErrorOr<NonnullOwnPtr<VideoDecoder>> create_video_decoder(VideoDecoderSelection, CodecID, ReadonlyBytes codec_initialization_data);

}
