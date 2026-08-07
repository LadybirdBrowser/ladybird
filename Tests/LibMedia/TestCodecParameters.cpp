/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/CodecParameters.h>
#include <LibTest/TestCase.h>

TEST_CASE(simple_codec_strings)
{
    struct TestCase {
        StringView codec_string;
        Media::CodecID codec_id;
    };
    for (auto test_case : {
             TestCase { "vp8"sv, Media::CodecID::VP8 },
             TestCase { "vp8.0"sv, Media::CodecID::VP8 },
             TestCase { "mp3"sv, Media::CodecID::MP3 },
             TestCase { "theora"sv, Media::CodecID::Theora },
             TestCase { "vorbis"sv, Media::CodecID::Vorbis },
             TestCase { "opus"sv, Media::CodecID::Opus },
             TestCase { "Opus"sv, Media::CodecID::Opus },
             TestCase { "flac"sv, Media::CodecID::FLAC },
             TestCase { "fLaC"sv, Media::CodecID::FLAC },
             TestCase { "pcm-u8"sv, Media::CodecID::U8 },
             TestCase { "pcm-s16"sv, Media::CodecID::S16LE },
             TestCase { "pcm-s24"sv, Media::CodecID::S24LE },
             TestCase { "pcm-s32"sv, Media::CodecID::S32LE },
             TestCase { "pcm-f32"sv, Media::CodecID::F32LE },
             TestCase { "alaw"sv, Media::CodecID::ALaw },
             TestCase { "ulaw"sv, Media::CodecID::MuLaw },
         }) {
        auto codec = Media::parse_codec_parameters_string(test_case.codec_string);
        EXPECT(codec.has_value());
        EXPECT_EQ(codec->codec_id(), test_case.codec_id);
        EXPECT(!codec->has_parameters());
    }
}

TEST_CASE(vp9_legacy_codec_string)
{
    for (auto codec_string : { "vp9"sv, "vp9.0"sv }) {
        auto codec = Media::parse_codec_parameters_string(codec_string);
        EXPECT(codec.has_value());
        EXPECT_EQ(codec->codec_id(), Media::CodecID::VP9);
        EXPECT(!codec->has_parameters());
    }
}

TEST_CASE(vp9_codec_parameters)
{
    auto codec = Media::parse_codec_parameters_string("vp09.02.10.10.01.09.16.09.01"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(codec->codec_id(), Media::CodecID::VP9);
    EXPECT(codec->has_parameters());

    auto const& parameters = codec->vp9_parameters().value();
    EXPECT_EQ(parameters.profile, 2);
    EXPECT_EQ(parameters.level, 10);
    EXPECT_EQ(parameters.bit_depth, 10);
    EXPECT(parameters.color_parameters.subsampling == Media::Subsampling::yuv420());
    EXPECT(parameters.color_parameters.cicp.color_primaries() == Media::ColorPrimaries::BT2020);
    EXPECT(parameters.color_parameters.cicp.transfer_characteristics() == Media::TransferCharacteristics::SMPTE2084);
    EXPECT(parameters.color_parameters.cicp.matrix_coefficients() == Media::MatrixCoefficients::BT2020NonConstantLuminance);
    EXPECT(parameters.color_parameters.cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full);

    codec = Media::parse_codec_parameters_string("vp09.00.41.08"sv);
    EXPECT(codec.has_value());
    EXPECT(codec->vp9_parameters()->color_parameters.subsampling == Media::Subsampling::yuv420());
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.color_primaries() == Media::ColorPrimaries::BT709);
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.transfer_characteristics() == Media::TransferCharacteristics::BT709);
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.matrix_coefficients() == Media::MatrixCoefficients::BT709);
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Studio);

    // A string may state Unspecified, which is preserved rather than resolved to the omitted-field defaults.
    codec = Media::parse_codec_parameters_string("vp09.00.41.08.01.02.02.02.00"sv);
    EXPECT(codec.has_value());
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.color_primaries() == Media::ColorPrimaries::Unspecified);
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.transfer_characteristics() == Media::TransferCharacteristics::Unspecified);
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.matrix_coefficients() == Media::MatrixCoefficients::Unspecified);

    codec = Media::parse_codec_parameters_string("vp09.01.41.08.03.22.18.14.00"sv);
    EXPECT(codec.has_value());
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.color_primaries() == Media::ColorPrimaries::EBU3213);
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.transfer_characteristics() == Media::TransferCharacteristics::HLG);
    EXPECT(codec->vp9_parameters()->color_parameters.cicp.matrix_coefficients() == Media::MatrixCoefficients::ICtCp);
}

TEST_CASE(invalid_vp9_codec_parameters)
{
    EXPECT(!Media::parse_codec_parameters_string("vp09"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41.08.01"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.04.41.08"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.42.08"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41.09"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41.08.04.01.01.01.00"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41.08.01.03.01.01.00"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41.08.01.01.03.01.00"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41.08.01.01.01.03.00"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("vp09.00.41.08.01.01.01.01.02"sv).has_value());
}

TEST_CASE(av1_codec_parameters)
{
    auto codec = Media::parse_codec_parameters_string("av01.0.05M.08"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(codec->codec_id(), Media::CodecID::AV1);
    EXPECT(codec->has_parameters());

    auto const& parameters = codec->av1_parameters().value();
    EXPECT_EQ(parameters.profile, 0);
    EXPECT_EQ(parameters.level, 5);
    EXPECT(parameters.tier == Media::Codecs::AV1::Tier::Main);
    EXPECT_EQ(parameters.bit_depth, 8);

    // Omitting the optional fields assumes the values that the codecs parameter table lists.
    EXPECT(!parameters.optional_fields.monochrome);
    EXPECT(parameters.optional_fields.subsampling == Media::Subsampling::yuv420());
    EXPECT_EQ(parameters.optional_fields.chroma_sample_position, 0);
    EXPECT(parameters.optional_fields.cicp.color_primaries() == Media::ColorPrimaries::BT709);
    EXPECT(parameters.optional_fields.cicp.transfer_characteristics() == Media::TransferCharacteristics::BT709);
    EXPECT(parameters.optional_fields.cicp.matrix_coefficients() == Media::MatrixCoefficients::BT709);
    EXPECT(parameters.optional_fields.cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Studio);

    codec = Media::parse_codec_parameters_string("av01.0.08M.10.0.110.09.16.09.0"sv);
    EXPECT(codec.has_value());
    auto const& optional_fields = codec->av1_parameters()->optional_fields;
    EXPECT(!optional_fields.monochrome);
    EXPECT(optional_fields.subsampling == Media::Subsampling::yuv420());
    EXPECT_EQ(optional_fields.chroma_sample_position, 0);
    EXPECT(optional_fields.cicp.color_primaries() == Media::ColorPrimaries::BT2020);
    EXPECT(optional_fields.cicp.transfer_characteristics() == Media::TransferCharacteristics::SMPTE2084);
    EXPECT(optional_fields.cicp.matrix_coefficients() == Media::MatrixCoefficients::BT2020NonConstantLuminance);
    EXPECT(optional_fields.cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Studio);

    // A string may state Unspecified, which is preserved rather than resolved to the omitted-field defaults.
    codec = Media::parse_codec_parameters_string("av01.0.05M.08.0.110.02.02.02.0"sv);
    EXPECT(codec.has_value());
    EXPECT(codec->av1_parameters()->optional_fields.cicp.color_primaries() == Media::ColorPrimaries::Unspecified);
    EXPECT(codec->av1_parameters()->optional_fields.cicp.transfer_characteristics() == Media::TransferCharacteristics::Unspecified);
    EXPECT(codec->av1_parameters()->optional_fields.cicp.matrix_coefficients() == Media::MatrixCoefficients::Unspecified);

    EXPECT(Media::parse_codec_parameters_string("av01.0.05M.08garbage"sv).has_value());
    EXPECT(Media::parse_codec_parameters_string("av01.0.08M.10.0.110.09.16.09.0garbage"sv).has_value());
}

TEST_CASE(invalid_av1_codec_parameters)
{
    EXPECT(!Media::parse_codec_parameters_string("av01"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.080"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.08.garbage"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.08a"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.08v"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.08M"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.08H"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.08.0"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.3.05M.08"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.2.08M.12.0.010.01.01.01.0"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.24M.08"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05H.08"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.05M.12"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("av01.0.08M.10.0.110.03.01.01.0"sv).has_value());
}

TEST_CASE(h264_codec_parameters)
{
    for (auto codec_string : { "avc1"sv, "avc3"sv }) {
        auto codec = Media::parse_codec_parameters_string(codec_string);
        EXPECT(codec.has_value());
        EXPECT_EQ(codec->codec_id(), Media::CodecID::H264);
        EXPECT(!codec->has_parameters());
    }

    for (auto codec_string : { "avc1.4D401E"sv, "avc1.4d401e"sv }) {
        auto codec = Media::parse_codec_parameters_string(codec_string);
        EXPECT(codec.has_value());
        EXPECT_EQ(codec->codec_id(), Media::CodecID::H264);
        EXPECT(codec->has_parameters());
        EXPECT_EQ(codec->h264_parameters()->profile_idc, 0x4d);
        EXPECT_EQ(codec->h264_parameters()->constraint_set_flags, 0x40);
        EXPECT_EQ(codec->h264_parameters()->level_idc, 0x1e);
    }
}

TEST_CASE(invalid_h264_codec_parameters)
{
    EXPECT(!Media::parse_codec_parameters_string("avc1."sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("avc1.64002"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("avc1.640028.1"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("avc1.64002g"sv).has_value());
}

TEST_CASE(h265_codec_parameters)
{
    for (auto codec_string : { "hvc1.1.6.L93.B0"sv, "hev1.1.6.L93.B0"sv }) {
        auto codec = Media::parse_codec_parameters_string(codec_string);
        EXPECT(codec.has_value());
        EXPECT_EQ(codec->codec_id(), Media::CodecID::H265);
        EXPECT(codec->has_parameters());

        auto const& parameters = codec->h265_parameters().value();
        EXPECT_EQ(parameters.profile_space, 0);
        EXPECT_EQ(parameters.profile_idc, 1);
        EXPECT_EQ(parameters.profile_compatibility_flags, 0x6u);
        EXPECT(!parameters.tier_flag);
        EXPECT_EQ(parameters.level_idc, 93);
        EXPECT_EQ(parameters.constraint_indicator_flags, (Array<u8, 6> { 0xb0, 0, 0, 0, 0, 0 }));
    }

    auto codec = Media::parse_codec_parameters_string("hvc1.A12.80000000.H255.90.12.34.56.78.9a"sv);
    EXPECT(codec.has_value());
    auto const& parameters = codec->h265_parameters().value();
    EXPECT_EQ(parameters.profile_space, 1);
    EXPECT_EQ(parameters.profile_idc, 12);
    EXPECT_EQ(parameters.profile_compatibility_flags, 0x80000000u);
    EXPECT(parameters.tier_flag);
    EXPECT_EQ(parameters.level_idc, 255);
    EXPECT_EQ(parameters.constraint_indicator_flags, (Array<u8, 6> { 0x90, 0x12, 0x34, 0x56, 0x78, 0x9a }));

    codec = Media::parse_codec_parameters_string("hvc1.2.4.L153"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(codec->h265_parameters()->constraint_indicator_flags, (Array<u8, 6> {}));

    codec = Media::parse_codec_parameters_string("hvc1.1.40000000.L60.80.0.0.0.0.0"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(codec->h265_parameters()->profile_idc, 1);
    EXPECT_EQ(codec->h265_parameters()->profile_compatibility_flags, 0x40000000u);
    EXPECT_EQ(codec->h265_parameters()->level_idc, 60);
    EXPECT_EQ(codec->h265_parameters()->constraint_indicator_flags, (Array<u8, 6> { 0x80, 0, 0, 0, 0, 0 }));
}

TEST_CASE(invalid_h265_codec_parameters)
{
    EXPECT(!Media::parse_codec_parameters_string("hvc1"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("hvc1.32.6.L93.B0"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("hvc1.1.123456789.L93.B0"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("hvc1.1.6.M93.B0"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("hvc1.1.6.L256.B0"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("hvc1.1.6.L93.B0.00.00.00.00.00.00"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("hvc1.1.6.L93.100"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("hvc1.1.6.L93.B0garbage"sv).has_value());
}

TEST_CASE(mpeg_4_audio_codec_parameters)
{
    auto codec = Media::parse_codec_parameters_string("mp4a.40"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(codec->codec_id(), Media::CodecID::AAC);
    EXPECT(codec->has_parameters());
    EXPECT_EQ(codec->aac_parameters()->object_type_indication, 0x40);
    EXPECT(!codec->aac_parameters()->audio_object_type.has_value());

    codec = Media::parse_codec_parameters_string("mp4a.40.02"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(codec->aac_parameters()->audio_object_type, 2u);

    codec = Media::parse_codec_parameters_string("mp4a.67"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(codec->codec_id(), Media::CodecID::AAC);
    EXPECT_EQ(codec->aac_parameters()->object_type_indication, 0x67);

    for (auto codec_string : { "mp4a.69"sv, "mp4a.6B"sv, "mp4a.6b"sv }) {
        codec = Media::parse_codec_parameters_string(codec_string);
        EXPECT(codec.has_value());
        EXPECT_EQ(codec->codec_id(), Media::CodecID::MP3);
        EXPECT(!codec->has_parameters());
    }
}

TEST_CASE(invalid_mpeg_4_audio_codec_parameters)
{
    EXPECT(!Media::parse_codec_parameters_string("mp4a"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("mp4a.4"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("mp4a.400"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("mp4a.40."sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("mp4a.40.2.1"sv).has_value());
    EXPECT(!Media::parse_codec_parameters_string("mp4a.67.2"sv).has_value());
}

TEST_CASE(fully_specified_codec_parameters)
{
    for (auto codec_string : { "mp4a.40"sv, "avc1"sv }) {
        auto codec = Media::parse_codec_parameters_string(codec_string);
        EXPECT(codec.has_value());
        EXPECT(!codec->is_fully_specified());
    }

    for (auto codec_string : { "mp4a.40.2"sv, "mp4a.67"sv, "av01.0.05M.08"sv, "avc1.4d401e"sv, "hvc1.1.6.L93.B0"sv, "vp9"sv, "vp09.00.41.08"sv, "opus"sv }) {
        auto codec = Media::parse_codec_parameters_string(codec_string);
        EXPECT(codec.has_value());
        EXPECT(codec->is_fully_specified());
    }
}
