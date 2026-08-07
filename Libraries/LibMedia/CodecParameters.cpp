/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/GenericShorthands.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/Codecs/MPEG4Audio.h>

namespace Media {

static Optional<CodecID> parse_simple_codec_string(StringView string)
{
    if (first_is_one_of(string, "vp8"sv, "vp8.0"sv))
        return CodecID::VP8;
    if (first_is_one_of(string, "vp9"sv, "vp9.0"sv))
        return CodecID::VP9;
    if (string == "mp3"sv)
        return CodecID::MP3;
    if (string == "theora"sv)
        return CodecID::Theora;
    if (string == "vorbis"sv)
        return CodecID::Vorbis;
    if (first_is_one_of(string, "opus"sv, "Opus"sv))
        return CodecID::Opus;
    if (first_is_one_of(string, "flac"sv, "fLaC"sv))
        return CodecID::FLAC;
    if (string == "pcm-u8"sv)
        return CodecID::U8;
    if (string == "pcm-s16"sv)
        return CodecID::S16LE;
    if (string == "pcm-s24"sv)
        return CodecID::S24LE;
    if (string == "pcm-s32"sv)
        return CodecID::S32LE;
    if (string == "pcm-f32"sv)
        return CodecID::F32LE;
    if (string == "alaw"sv)
        return CodecID::ALaw;
    if (string == "ulaw"sv)
        return CodecID::MuLaw;
    return {};
}

Optional<ParsedCodec> parse_codec_parameters_string(StringView string)
{
    if (auto codec_id = parse_simple_codec_string(string); codec_id.has_value())
        return ParsedCodec { *codec_id };

    GenericLexer lexer { string };
    if (lexer.consume_specific("mp4a"sv)) {
        auto codec = Codecs::MPEG4Audio::parse_codec_parameters(lexer);
        if (!codec.has_value())
            return {};
        if (codec->has<Codecs::AAC::Parameters>())
            return ParsedCodec { codec->get<Codecs::AAC::Parameters>() };
        return ParsedCodec { CodecID::MP3 };
    }
    if (lexer.consume_specific("avc1"sv) || lexer.consume_specific("avc3"sv)) {
        if (lexer.is_eof())
            return ParsedCodec { CodecID::H264 };
        auto parameters = Codecs::H264::parse_codec_parameters(lexer);
        if (!parameters.has_value())
            return {};
        return ParsedCodec { *parameters };
    }
    if (lexer.consume_specific("hev1"sv) || lexer.consume_specific("hvc1"sv)) {
        auto parameters = Codecs::H265::parse_codec_parameters(lexer);
        if (!parameters.has_value())
            return {};
        return ParsedCodec { *parameters };
    }
    if (lexer.consume_specific("vp09"sv)) {
        auto parameters = Codecs::VP9::parse_codec_parameters(lexer);
        if (!parameters.has_value())
            return {};
        return ParsedCodec { *parameters };
    }
    if (lexer.consume_specific("av01"sv)) {
        auto parameters = Codecs::AV1::parse_codec_parameters(lexer);
        if (!parameters.has_value())
            return {};
        return ParsedCodec { *parameters };
    }

    return {};
}

}
