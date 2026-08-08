/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderRegistry.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/MediaSupport.h>

namespace Media {

// A WAVE container's codecs parameter is a format tag in hexadecimal rather than an RFC 6381 string, specifying the
// PCM format.
static Optional<CodecID> wave_codec_from_codec_parameter(StringView codec_parameter)
{
    auto format_tag = codec_parameter.to_number<u16>(TrimWhitespace::No, 16);
    if (!format_tag.has_value())
        return {};

    switch (*format_tag) {
    case 0x0001:
        return CodecID::S16LE;
    case 0x0003:
        return CodecID::F32LE;
    case 0x0006:
        return CodecID::ALaw;
    case 0x0007:
        return CodecID::MuLaw;
    default:
        return {};
    }
}

// A container whose MIME type names its only codec needs no codecs parameter to be identified.
Optional<CodecID> codec_implied_by_file_container(ContainerMimeType mime_type)
{
    switch (mime_type.container_id) {
    case ContainerID::ADTS:
        return CodecID::AAC;
    case ContainerID::FLAC:
        return CodecID::FLAC;
    case ContainerID::MPEGAudio:
    case ContainerID::ISOBMFF:
    case ContainerID::Matroska:
    case ContainerID::Ogg:
    case ContainerID::WAV:
    case ContainerID::WebM:
        return {};
    }
    VERIFY_NOT_REACHED();
}

MediaSupportInfo file_media_support(MimeTypeView const& mime_type)
{
    auto container = container_mime_type_from_mime_type(mime_type.type, mime_type.subtype);
    if (!container.has_value() || !is_supported_file_container(*container))
        return {};

    auto codecs_iterator = mime_type.parameters.find("codecs"sv);
    if (codecs_iterator == mime_type.parameters.end()) {
        auto implied_codec = codec_implied_by_file_container(*container);
        if (!implied_codec.has_value())
            return { MediaSupport::Maybe };

        auto capabilities = decoder_capabilities(ParsedCodec { *implied_codec });
        if (!capabilities.has_value())
            return {};
        return { MediaSupport::Probably, *capabilities };
    }

    auto codec_strings = codecs_iterator->value.bytes_as_string_view().split_view(',', SplitBehavior::KeepEmpty);
    if (codec_strings.is_empty())
        return {};

    if (container->container_id == ContainerID::WAV) {
        if (codec_strings.size() != 1)
            return {};
        auto codec_id = wave_codec_from_codec_parameter(codec_strings[0].trim_whitespace());
        if (!codec_id.has_value())
            return {};
        auto capabilities = decoder_capabilities(ParsedCodec { *codec_id });
        if (!capabilities.has_value())
            return {};
        return { MediaSupport::Probably, *capabilities };
    }

    MediaSupportInfo info { MediaSupport::Probably, { .smooth = true, .power_efficient = true } };
    for (auto codec_string : codec_strings) {
        auto codec = parse_codec_parameters_string(codec_string.trim_whitespace());
        if (!codec.has_value() || !is_codec_supported_in_file_container(*container, codec->codec_id()))
            return {};

        auto capabilities = decoder_capabilities(*codec);
        if (!capabilities.has_value())
            return {};

        info.capabilities.smooth &= capabilities->smooth;
        info.capabilities.power_efficient &= capabilities->power_efficient;
        if (!codec->is_fully_specified())
            info.support = MediaSupport::Maybe;
    }
    return info;
}

}
