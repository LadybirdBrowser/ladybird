/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/GenericShorthands.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/Codecs/MPEG4Audio.h>
#include <LibMediaCodecs/CodedFrame.h>

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
static Optional<ParsedCodec> parse_configuration_record(CodecID codec_id, ReadonlyBytes configuration_record)
{
    if (configuration_record.is_empty())
        return {};

    switch (codec_id) {
    case CodecID::AV1:
        if (auto parameters = Codecs::AV1::parse_configuration_record(configuration_record); parameters.has_value())
            return ParsedCodec { *parameters };
        return {};
    case CodecID::H264:
        if (auto parameters = Codecs::H264::parse_configuration_record(configuration_record); parameters.has_value())
            return ParsedCodec { *parameters };
        return {};
    case CodecID::H265:
        if (auto parameters = Codecs::H265::parse_configuration_record(configuration_record); parameters.has_value())
            return ParsedCodec { *parameters };
        return {};
    case CodecID::VP9:
        if (auto parameters = Codecs::VP9::parse_configuration_record(configuration_record); parameters.has_value())
            return ParsedCodec { *parameters };
        return {};
    default:
        return {};
    }
}

static Optional<ParsedCodec> parse_codec_config_from_frame_data(CodecID codec_id, ReadonlyBytes coded_data)
{
    switch (codec_id) {
    case CodecID::VP9: {
        auto header = Codecs::VP9::parse_frame_header(coded_data);
        if (!header.has_value())
            return {};
        // A frame header says nothing about the level, which bounds only what a decoder must keep up with rather than
        // what it must understand, and so is not what a decoder is chosen on.
        return ParsedCodec { Codecs::VP9::Parameters {
            .profile = header->profile,
            .level = 0,
            .bit_depth = header->bit_depth,
            .color_parameters = header->color_parameters,
        } };
    }
    default:
        return {};
    }
}

ParsedCodec parsed_codec_for_coded_frame(CodedFrame const& coded_frame, ParsedCodec const& container_codec)
{
    auto codec_id = coded_frame.codec_id();

    if (auto frame_config = coded_frame.new_codec_configuration(); frame_config.has_value()) {
        if (auto parsed_frame_config = parse_configuration_record(codec_id, *frame_config); parsed_frame_config.has_value())
            return parsed_frame_config.release_value();
    }

    if (container_codec.codec_id() == codec_id && container_codec.has_parameters())
        return container_codec;

    if (auto parsed_bitstream_config = parse_codec_config_from_frame_data(codec_id, coded_frame.data()); parsed_bitstream_config.has_value())
        return parsed_bitstream_config.release_value();

    return ParsedCodec { codec_id };
}

ParsedCodec normalize_parsed_codec_for_support_keying(ParsedCodec const& codec)
{
    switch (codec.codec_id()) {
    case CodecID::VP9: {
        auto parameters = codec.vp9_parameters().value_or({ .profile = 0, .level = 0, .bit_depth = 8, .color_parameters = {} });
        parameters.level = 0;
        parameters.color_parameters.cicp = {};
        return ParsedCodec { parameters };
    }
    case CodecID::AV1: {
        auto parameters = codec.av1_parameters().value_or({ .profile = 0, .level = 0, .tier = Codecs::AV1::Tier::Main, .bit_depth = 8, .optional_fields = {} });
        parameters.level = 0;
        parameters.tier = Codecs::AV1::Tier::Main;
        parameters.optional_fields.cicp = {};
        return ParsedCodec { parameters };
    }
    case CodecID::H264: {
        Optional<Codecs::H264::Profile> profile = Codecs::H264::Profile::Main;
        if (auto parameters = codec.h264_parameters(); parameters.has_value())
            profile = parameters->profile();
        if (!profile.has_value())
            return codec;
        return ParsedCodec { Codecs::H264::canonical_parameters_for_profile(*profile) };
    }
    case CodecID::H265: {
        Optional<Codecs::H265::Profile> profile = Codecs::H265::Profile::Main;
        if (auto parameters = codec.h265_parameters(); parameters.has_value())
            profile = parameters->profile();
        if (!profile.has_value())
            return codec;
        return ParsedCodec { Codecs::H265::canonical_parameters_for_profile(*profile) };
    }
    default:
        return codec;
    }
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::ParsedCodec const& codec)
{
    TRY(encoder.encode(codec.codec_id()));
    TRY(encoder.encode(codec.has_parameters()));
    if (!codec.has_parameters())
        return {};

    switch (codec.codec_id()) {
    case Media::CodecID::AAC: {
        auto const& parameters = *codec.aac_parameters();
        TRY(encoder.encode(parameters.object_type_indication));
        TRY(encoder.encode(parameters.audio_object_type));
        return {};
    }
    case Media::CodecID::H264: {
        auto const& parameters = *codec.h264_parameters();
        TRY(encoder.encode(parameters.profile_idc));
        TRY(encoder.encode(parameters.constraint_set_flags));
        TRY(encoder.encode(parameters.level_idc));
        return {};
    }
    case Media::CodecID::H265: {
        auto const& parameters = *codec.h265_parameters();
        for (auto flags : parameters.constraint_indicator_flags)
            TRY(encoder.encode(flags));
        TRY(encoder.encode(parameters.profile_compatibility_flags));
        TRY(encoder.encode(parameters.profile_space));
        TRY(encoder.encode(parameters.profile_idc));
        TRY(encoder.encode(parameters.level_idc));
        TRY(encoder.encode(parameters.tier_flag));
        return {};
    }
    case Media::CodecID::AV1: {
        auto const& parameters = *codec.av1_parameters();
        TRY(encoder.encode(parameters.profile));
        TRY(encoder.encode(parameters.level));
        TRY(encoder.encode(parameters.tier));
        TRY(encoder.encode(parameters.bit_depth));
        TRY(encoder.encode(parameters.optional_fields.monochrome));
        TRY(encoder.encode(parameters.optional_fields.subsampling.x()));
        TRY(encoder.encode(parameters.optional_fields.subsampling.y()));
        TRY(encoder.encode(parameters.optional_fields.chroma_sample_position));
        TRY(encoder.encode(parameters.optional_fields.cicp));
        return {};
    }
    case Media::CodecID::VP9: {
        auto const& parameters = *codec.vp9_parameters();
        TRY(encoder.encode(parameters.profile));
        TRY(encoder.encode(parameters.level));
        TRY(encoder.encode(parameters.bit_depth));
        TRY(encoder.encode(parameters.color_parameters.subsampling.x()));
        TRY(encoder.encode(parameters.color_parameters.subsampling.y()));
        TRY(encoder.encode(parameters.color_parameters.cicp));
        return {};
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

template<>
ErrorOr<Media::ParsedCodec> decode(Decoder& decoder)
{
    auto codec_id = TRY(decoder.decode<Media::CodecID>());
    if (!TRY(decoder.decode<bool>()))
        return Media::ParsedCodec { codec_id };

    switch (codec_id) {
    case Media::CodecID::AAC: {
        Media::Codecs::AAC::Parameters parameters {};
        parameters.object_type_indication = TRY(decoder.decode<u8>());
        parameters.audio_object_type = TRY(decoder.decode<Optional<u32>>());
        return Media::ParsedCodec { parameters };
    }
    case Media::CodecID::H264: {
        Media::Codecs::H264::Parameters parameters;
        parameters.profile_idc = TRY(decoder.decode<u8>());
        parameters.constraint_set_flags = TRY(decoder.decode<u8>());
        parameters.level_idc = TRY(decoder.decode<u8>());
        return Media::ParsedCodec { parameters };
    }
    case Media::CodecID::H265: {
        Media::Codecs::H265::Parameters parameters;
        for (auto& flags : parameters.constraint_indicator_flags)
            flags = TRY(decoder.decode<u8>());
        parameters.profile_compatibility_flags = TRY(decoder.decode<u32>());
        parameters.profile_space = TRY(decoder.decode<u8>());
        parameters.profile_idc = TRY(decoder.decode<u8>());
        parameters.level_idc = TRY(decoder.decode<u8>());
        parameters.tier_flag = TRY(decoder.decode<bool>());
        return Media::ParsedCodec { parameters };
    }
    case Media::CodecID::AV1: {
        Media::Codecs::AV1::Parameters parameters;
        parameters.profile = TRY(decoder.decode<u8>());
        parameters.level = TRY(decoder.decode<u8>());
        parameters.tier = TRY(decoder.decode<Media::Codecs::AV1::Tier>());
        parameters.bit_depth = TRY(decoder.decode<u8>());
        parameters.optional_fields.monochrome = TRY(decoder.decode<bool>());
        auto subsampling_x = TRY(decoder.decode<bool>());
        auto subsampling_y = TRY(decoder.decode<bool>());
        parameters.optional_fields.subsampling = { subsampling_x, subsampling_y };
        parameters.optional_fields.chroma_sample_position = TRY(decoder.decode<u8>());
        parameters.optional_fields.cicp = TRY(decoder.decode<Media::CodingIndependentCodePoints>());
        return Media::ParsedCodec { parameters };
    }
    case Media::CodecID::VP9: {
        Media::Codecs::VP9::Parameters parameters;
        parameters.profile = TRY(decoder.decode<u8>());
        parameters.level = TRY(decoder.decode<u8>());
        parameters.bit_depth = TRY(decoder.decode<u8>());
        auto subsampling_x = TRY(decoder.decode<bool>());
        auto subsampling_y = TRY(decoder.decode<bool>());
        parameters.color_parameters.subsampling = { subsampling_x, subsampling_y };
        parameters.color_parameters.cicp = TRY(decoder.decode<Media::CodingIndependentCodePoints>());
        return Media::ParsedCodec { parameters };
    }
    default:
        return Error::from_string_literal("Codec parameters for a codec that has none");
    }
}

}
