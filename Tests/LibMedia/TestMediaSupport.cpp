/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderRegistry.h>
#include <LibMedia/MediaSupport.h>
#include <LibTest/TestCase.h>

static Media::MediaSupportInfo file_media_support(StringView type, StringView subtype, Optional<StringView> codecs = {})
{
    OrderedHashMap<String, String> parameters;
    if (codecs.has_value())
        parameters.set("codecs"_string, MUST(String::from_utf8(*codecs)));
    return Media::file_media_support({ type, subtype, parameters });
}

TEST_CASE(file_media_support_without_codecs)
{
    EXPECT_EQ(file_media_support("video"sv, "webm"sv), (Media::MediaSupportInfo { Media::MediaSupport::Maybe }));
    EXPECT_EQ(file_media_support("audio"sv, "wav"sv), (Media::MediaSupportInfo { Media::MediaSupport::Maybe }));
    EXPECT_EQ(file_media_support("video"sv, "unknown"sv), (Media::MediaSupportInfo {}));
}

TEST_CASE(file_media_support_with_codecs)
{
    EXPECT_EQ(file_media_support("video"sv, "webm"sv, "vp09.00.10.08"sv).support, Media::MediaSupport::Probably);
    EXPECT_EQ(file_media_support("video"sv, "webm"sv, "vp9"sv).support, Media::MediaSupport::Probably);
    EXPECT_EQ(file_media_support("video"sv, "webm"sv, "avc1.640028"sv), (Media::MediaSupportInfo {}));
    EXPECT_EQ(file_media_support("audio"sv, "mp4"sv, "avc1.640028"sv), (Media::MediaSupportInfo {}));

    // H.264 and AAC come from platform decoders or a system FFmpeg, so only what this machine can decode is expected.
    auto expect_support = [](Media::MediaSupportInfo const& info, std::initializer_list<Media::ParsedCodec> codecs, Media::MediaSupport support_when_decodable) {
        auto decodable = true;
        for (auto const& codec : codecs)
            decodable &= Media::decoder_capabilities(codec).has_value();
        if (decodable)
            EXPECT_EQ(info.support, support_when_decodable);
        else
            EXPECT_EQ(info, (Media::MediaSupportInfo {}));
    };
    auto codec = [](StringView codec_string) { return *Media::parse_codec_parameters_string(codec_string); };
    expect_support(file_media_support("video"sv, "mp4"sv, "avc1.640028, mp4a.40.2"sv), { codec("avc1.640028"sv), codec("mp4a.40.2"sv) }, Media::MediaSupport::Probably);
    expect_support(file_media_support("video"sv, "mp4"sv, "avc1, mp4a.40.2"sv), { codec("avc1"sv), codec("mp4a.40.2"sv) }, Media::MediaSupport::Maybe);
    expect_support(file_media_support("audio"sv, "mp4"sv, "mp4a.40"sv), { codec("mp4a.40"sv) }, Media::MediaSupport::Maybe);
    expect_support(file_media_support("audio"sv, "mp4"sv, "mp4a.67"sv), { codec("mp4a.67"sv) }, Media::MediaSupport::Probably);
}

TEST_CASE(wave_file_media_support)
{
    for (auto codec_parameter : { "1"sv, "0001"sv, "3"sv, "6"sv, "7"sv })
        EXPECT_EQ(file_media_support("audio"sv, "wav"sv, codec_parameter).support, Media::MediaSupport::Probably);

    for (auto codec_parameter : { ""sv, "2"sv, "fffe"sv, "0x1"sv, "xyz"sv, "1,3"sv })
        EXPECT_EQ(file_media_support("audio"sv, "wav"sv, codec_parameter), (Media::MediaSupportInfo {}));
}
