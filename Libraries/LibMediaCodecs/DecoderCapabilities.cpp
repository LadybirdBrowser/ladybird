/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/AudioDecoderRegistry.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>

namespace Media {

Optional<DecoderCapabilities> decoder_capabilities(ParsedCodec const& codec)
{
    switch (track_type_from_codec_id(codec.codec_id())) {
    case TrackType::Audio:
        return audio_decoder_capabilities(codec);
    case TrackType::Video:
        return video_decoder_capabilities(codec);
    case TrackType::Subtitles:
    case TrackType::Unknown:
        return {};
    }
    VERIFY_NOT_REACHED();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::DecoderCapabilities const& capabilities)
{
    TRY(encoder.encode(capabilities.smooth));
    TRY(encoder.encode(capabilities.power_efficient));
    return {};
}

template<>
ErrorOr<Media::DecoderCapabilities> decode(Decoder& decoder)
{
    Media::DecoderCapabilities capabilities;
    capabilities.smooth = TRY(decoder.decode<bool>());
    capabilities.power_efficient = TRY(decoder.decode<bool>());
    return capabilities;
}

}
