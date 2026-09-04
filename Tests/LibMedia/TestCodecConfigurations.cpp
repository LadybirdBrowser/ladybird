/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/Codecs/AAC.h>
#include <LibMedia/Codecs/AV1.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Codecs/H264.h>
#include <LibMedia/Codecs/H265.h>
#include <LibMedia/Codecs/Opus.h>
#include <LibMedia/Codecs/VP9.h>
#include <LibTest/TestCase.h>

TEST_CASE(opus_isobmff_configuration_is_normalized_to_opus_head)
{
    Array<u8, 15> configuration {
        0, 2, 0x01, 0x02, 0x00, 0x00, 0xbb, 0x80, 0xff, 0x00, 1, 1, 1, 0, 1
    };
    Array<u8, 23> expected {
        'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
        1, 2, 0x02, 0x01, 0x80, 0xbb, 0x00, 0x00, 0x00, 0xff, 1, 1, 1, 0, 1
    };

    auto initialization_data = MUST(Media::Codecs::Opus::codec_initialization_data_from_isobmff_configuration(configuration));
    EXPECT(initialization_data.span() == expected.span());
}

TEST_CASE(opus_isobmff_configuration_rejects_invalid_wrapper)
{
    Array<u8, 11> configuration { 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    auto unsupported_version = configuration;
    unsupported_version[0] = 1;
    EXPECT(Media::Codecs::Opus::codec_initialization_data_from_isobmff_configuration(unsupported_version).is_error());
    EXPECT(Media::Codecs::Opus::codec_initialization_data_from_isobmff_configuration(configuration.span().trim(10)).is_error());
}

TEST_CASE(flac_isobmff_configuration_is_normalized_to_native_metadata)
{
    Array<u8, 49> configuration {
        0, 0, 0, 0,
        0x00, 0x00, 0x00, 0x22,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
        0x84, 0x00, 0x00, 0x03,
        'a', 'b', 'c'
    };

    auto initialization_data = MUST(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(configuration));
    EXPECT(initialization_data.span().slice(0, 4) == "fLaC"sv.bytes());
    EXPECT(initialization_data.span().slice(4) == configuration.span().slice(4));
}

TEST_CASE(flac_isobmff_configuration_rejects_invalid_wrapper)
{
    Array<u8, 42> configuration {
        0, 0, 0, 0,
        0x80, 0x00, 0x00, 0x22,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33
    };

    auto unsupported_version = configuration;
    unsupported_version[0] = 1;
    EXPECT(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(unsupported_version).is_error());

    auto unsupported_flags = configuration;
    unsupported_flags[3] = 1;
    EXPECT(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(unsupported_flags).is_error());
    EXPECT(Media::Codecs::FLAC::codec_initialization_data_from_isobmff_configuration(configuration.span().trim(3)).is_error());
}

TEST_CASE(h264_configuration_record_matches_its_codec_string)
{
    Array<u8, 4> record { 1, 0x64, 0x00, 0x1f };

    auto parameters = Media::Codecs::H264::parse_configuration_record(record);
    EXPECT(parameters.has_value());

    auto codec = Media::parse_codec_parameters_string("avc1.64001F"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(*parameters, *codec->h264_parameters());
}

TEST_CASE(h264_configuration_record_rejects_invalid_wrapper)
{
    Array<u8, 4> record { 1, 0x64, 0x00, 0x1f };

    auto unsupported_version = record;
    unsupported_version[0] = 0;
    EXPECT(!Media::Codecs::H264::parse_configuration_record(unsupported_version).has_value());
    EXPECT(!Media::Codecs::H264::parse_configuration_record(record.span().trim(3)).has_value());
}

TEST_CASE(h265_configuration_record_matches_its_codec_string)
{
    Array<u8, 13> record {
        1,
        0x01,
        0x60, 0x00, 0x00, 0x00,
        0xb0, 0x00, 0x00, 0x00, 0x00, 0x00,
        93
    };

    auto parameters = Media::Codecs::H265::parse_configuration_record(record);
    EXPECT(parameters.has_value());

    auto codec = Media::parse_codec_parameters_string("hvc1.1.6.L93.B0"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(*parameters, *codec->h265_parameters());
}

TEST_CASE(h265_configuration_record_reads_the_profile_space_and_tier)
{
    Array<u8, 13> record {
        1,
        0x6c,
        0x00, 0x00, 0x00, 0x01,
        0x90, 0x12, 0x34, 0x56, 0x78, 0x9a,
        255
    };

    auto parameters = Media::Codecs::H265::parse_configuration_record(record);
    EXPECT(parameters.has_value());

    auto codec = Media::parse_codec_parameters_string("hvc1.A12.80000000.H255.90.12.34.56.78.9a"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(*parameters, *codec->h265_parameters());
}

TEST_CASE(h265_configuration_record_rejects_invalid_wrapper)
{
    Array<u8, 13> record { 1, 0x01, 0x60, 0, 0, 0, 0xb0, 0, 0, 0, 0, 0, 93 };

    auto unsupported_version = record;
    unsupported_version[0] = 2;
    EXPECT(!Media::Codecs::H265::parse_configuration_record(unsupported_version).has_value());
    EXPECT(!Media::Codecs::H265::parse_configuration_record(record.span().trim(12)).has_value());
}

TEST_CASE(vp9_configuration_record_matches_its_codec_string)
{
    Array<u8, 8> record {
        2,
        31,
        (10 << 4) | (1 << 1) | 0,
        9, 16, 9,
        0, 0
    };

    auto parameters = Media::Codecs::VP9::parse_configuration_record(record);
    EXPECT(parameters.has_value());

    auto codec = Media::parse_codec_parameters_string("vp09.02.31.10.01.09.16.09.00"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(*parameters, *codec->vp9_parameters());
}

TEST_CASE(vp9_configuration_record_rejects_invalid_records)
{
    Array<u8, 8> record { 2, 31, (10 << 4) | (1 << 1) | 0, 9, 16, 9, 0, 0 };

    EXPECT(!Media::Codecs::VP9::parse_configuration_record(record.span().trim(7)).has_value());

    // The record carries no initialization data for VP9.
    auto with_initialization_data = record;
    with_initialization_data[7] = 4;
    EXPECT(!Media::Codecs::VP9::parse_configuration_record(with_initialization_data).has_value());
}

TEST_CASE(vp9_configuration_record_rejects_impossible_color_configurations)
{
    Array<u8, 8> record { 2, 31, (10 << 4) | (1 << 1) | 0, 9, 16, 9, 0, 0 };

    // Profile 2 cannot be 8-bit.
    auto mismatched_bit_depth = record;
    mismatched_bit_depth[2] = (8 << 4) | (1 << 1) | 0;
    EXPECT(!Media::Codecs::VP9::parse_configuration_record(mismatched_bit_depth).has_value());

    // Reserved chroma subsampling values have no mapping.
    auto reserved_subsampling = record;
    reserved_subsampling[2] = (10 << 4) | (5 << 1) | 0;
    EXPECT(!Media::Codecs::VP9::parse_configuration_record(reserved_subsampling).has_value());

    // Identity matrix coefficients require 4:4:4.
    auto identity_matrix = record;
    identity_matrix[5] = 0;
    EXPECT(!Media::Codecs::VP9::parse_configuration_record(identity_matrix).has_value());
}

TEST_CASE(av1_configuration_record_matches_its_codec_string)
{
    // A record whose configuration OBUs hold a sequence header describing BT.709.
    Array<u8, 21> record {
        0x81, 0x08, 0x0c, 0x00,
        0x0a, 0x0f, 0x00, 0x00, 0x00, 0x43, 0xfc, 0x1d, 0xfc, 0x10, 0xdd, 0xc2, 0x79, 0x90, 0x10, 0x10, 0x10
    };

    auto parameters = Media::Codecs::AV1::parse_configuration_record(record);
    EXPECT(parameters.has_value());

    auto codec = Media::parse_codec_parameters_string("av01.0.08M.08.0.110.01.01.01.0"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(*parameters, *codec->av1_parameters());
}

TEST_CASE(av1_configuration_record_reads_a_reduced_still_picture_sequence_header)
{
    // The same record, but with a reduced still picture header describing BT.2020 PQ.
    Array<u8, 17> record {
        0x81, 0x08, 0x0c, 0x00,
        0x0a, 0x0b, 0x0a, 0x3f, 0xc1, 0xdf, 0xc1, 0x0d, 0xfb, 0x21, 0x22, 0x01, 0x20
    };

    auto parameters = Media::Codecs::AV1::parse_configuration_record(record);
    EXPECT(parameters.has_value());
    EXPECT(parameters->optional_fields.cicp.color_primaries() == Media::ColorPrimaries::BT2020);
    EXPECT(parameters->optional_fields.cicp.transfer_characteristics() == Media::TransferCharacteristics::SMPTE2084);
    EXPECT(parameters->optional_fields.cicp.matrix_coefficients() == Media::MatrixCoefficients::BT2020NonConstantLuminance);
    EXPECT(parameters->optional_fields.cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Studio);
}

TEST_CASE(av1_configuration_record_without_configuration_obus_reports_unspecified_color)
{
    Array<u8, 4> record { 0x81, 0x08, 0x0c, 0x00 };

    auto parameters = Media::Codecs::AV1::parse_configuration_record(record);
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->profile, 0);
    EXPECT_EQ(parameters->level, 8);
    EXPECT_EQ(parameters->bit_depth, 8);
    EXPECT(parameters->optional_fields.subsampling == Media::Subsampling::yuv420());
    EXPECT(parameters->optional_fields.cicp.color_primaries() == Media::ColorPrimaries::Unspecified);
    EXPECT(parameters->optional_fields.cicp.transfer_characteristics() == Media::TransferCharacteristics::Unspecified);
    EXPECT(parameters->optional_fields.cicp.matrix_coefficients() == Media::MatrixCoefficients::Unspecified);
}

TEST_CASE(av1_sequence_header_without_a_color_description_reports_unspecified_color)
{
    // The same sequence header, but with color_description_present_flag cleared.
    Array<u8, 14> record {
        0x81, 0x08, 0x0c, 0x00,
        0x0a, 0x08, 0x0a, 0x3f, 0xc1, 0xdf, 0xc1, 0x0d, 0xfb, 0x00
    };

    auto parameters = Media::Codecs::AV1::parse_configuration_record(record);
    EXPECT(parameters.has_value());
    EXPECT(parameters->optional_fields.cicp.color_primaries() == Media::ColorPrimaries::Unspecified);
    EXPECT(parameters->optional_fields.cicp.transfer_characteristics() == Media::TransferCharacteristics::Unspecified);
    EXPECT(parameters->optional_fields.cicp.matrix_coefficients() == Media::MatrixCoefficients::Unspecified);
}

TEST_CASE(av1_configuration_record_rejects_invalid_wrapper)
{
    Array<u8, 4> record { 0x81, 0x08, 0x0c, 0x00 };

    auto missing_marker = record;
    missing_marker[0] = 0x01;
    EXPECT(!Media::Codecs::AV1::parse_configuration_record(missing_marker).has_value());

    auto unsupported_version = record;
    unsupported_version[0] = 0x82;
    EXPECT(!Media::Codecs::AV1::parse_configuration_record(unsupported_version).has_value());

    EXPECT(!Media::Codecs::AV1::parse_configuration_record(record.span().trim(3)).has_value());
}

TEST_CASE(aac_configuration_record_matches_its_codec_string)
{
    // AAC-LC at 44100 Hz, stereo.
    Array<u8, 2> audio_specific_config { 0x12, 0x10 };

    auto parameters = Media::Codecs::AAC::parse_configuration_record(audio_specific_config, 0x40);
    EXPECT(parameters.has_value());

    auto codec = Media::parse_codec_parameters_string("mp4a.40.2"sv);
    EXPECT(codec.has_value());
    EXPECT_EQ(*parameters, *codec->aac_parameters());
}

TEST_CASE(aac_configuration_record_reads_an_escaped_audio_object_type)
{
    // An audio object type of 31 escapes to six further bits holding the type less 32.
    Array<u8, 2> audio_specific_config { 0xf8, 0x20 };

    auto parameters = Media::Codecs::AAC::parse_configuration_record(audio_specific_config, 0x40);
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->audio_object_type, 33u);
}

TEST_CASE(aac_configuration_record_carries_the_object_type_indication_it_is_given)
{
    Array<u8, 2> audio_specific_config { 0x12, 0x10 };

    auto parameters = Media::Codecs::AAC::parse_configuration_record(audio_specific_config, 0x67);
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->object_type_indication, 0x67);
    EXPECT(parameters->is_fully_specified());
}

TEST_CASE(aac_configuration_record_rejects_a_truncated_config)
{
    EXPECT(!Media::Codecs::AAC::parse_configuration_record({}, 0x40).has_value());

    // The escape needs six more bits than the single byte holds.
    Array<u8, 1> escaped_without_extension { 0xf8 };
    EXPECT(!Media::Codecs::AAC::parse_configuration_record(escaped_without_extension, 0x40).has_value());
}

TEST_CASE(vp9_frame_header_reads_a_keyframe_format)
{
    // The head of vp9_in_webm.webm's keyframe, whose color space field is unknown.
    Array<u8, 16> keyframe {
        0x82, 0x49, 0x83, 0x42, 0x00, 0x35, 0x50, 0x1d, 0xf6, 0x12, 0x38, 0x24, 0x1c, 0x18, 0xb8, 0x10
    };

    auto header = Media::Codecs::VP9::parse_frame_header(keyframe);
    EXPECT(header.has_value());
    EXPECT_EQ(header->size, Gfx::IntSize(854, 480));
    EXPECT_EQ(header->profile, 0);
    EXPECT_EQ(header->bit_depth, 8);
    EXPECT(header->color_parameters.subsampling.x());
    EXPECT(header->color_parameters.subsampling.y());
    EXPECT_EQ(to_underlying(header->color_parameters.cicp.matrix_coefficients()), to_underlying(Media::MatrixCoefficients::Unspecified));

    // VP9 can express neither primaries nor transfer characteristics, so the container supplies them.
    EXPECT_EQ(to_underlying(header->color_parameters.cicp.color_primaries()), to_underlying(Media::ColorPrimaries::Unspecified));
    EXPECT_EQ(to_underlying(header->color_parameters.cicp.transfer_characteristics()), to_underlying(Media::TransferCharacteristics::Unspecified));
}

TEST_CASE(vp9_frame_header_reads_a_stated_matrix)
{
    // The head of big_buck_bunny_5s.webm's keyframe, which states BT.709.
    Array<u8, 16> keyframe {
        0x82, 0x49, 0x83, 0x42, 0x40, 0x27, 0xf0, 0x16, 0x76, 0x00, 0x38, 0x24, 0x1c, 0x19, 0x72, 0x10
    };

    auto header = Media::Codecs::VP9::parse_frame_header(keyframe);
    EXPECT(header.has_value());
    EXPECT_EQ(header->size, Gfx::IntSize(640, 360));
    EXPECT_EQ(to_underlying(header->color_parameters.cicp.matrix_coefficients()), to_underlying(Media::MatrixCoefficients::BT709));
}

TEST_CASE(vp9_frame_header_reads_the_last_frame_of_a_superframe)
{
    static constexpr Array<u8, 16> first { 0x82, 0x49, 0x83, 0x42, 0x00, 0x35, 0x50, 0x1d, 0xf6, 0x12, 0x38, 0x24, 0x1c, 0x18, 0xb8, 0x10 };
    static constexpr Array<u8, 16> second { 0x82, 0x49, 0x83, 0x42, 0x40, 0x27, 0xf0, 0x16, 0x76, 0x00, 0x38, 0x24, 0x1c, 0x19, 0x72, 0x10 };

    // Two frames behind an index of one size byte each, so the format is the second frame's.
    Vector<u8> superframe;
    superframe.append(first.data(), first.size());
    superframe.append(second.data(), second.size());
    u8 marker = 0xc0 | 0x1;
    superframe.append(marker);
    superframe.append(static_cast<u8>(first.size()));
    superframe.append(static_cast<u8>(second.size()));
    superframe.append(marker);

    auto header = Media::Codecs::VP9::parse_frame_header(superframe);
    EXPECT(header.has_value());
    EXPECT_EQ(header->size, Gfx::IntSize(640, 360));
}

TEST_CASE(vp9_frame_header_rejects_frames_that_describe_no_format)
{
    // An inter frame takes its size from a reference rather than coding one.
    Array<u8, 4> inter_frame { 0x86, 0x00, 0x00, 0x00 };
    EXPECT(!Media::Codecs::VP9::parse_frame_header(inter_frame).has_value());

    // A frame that only redisplays a reference. The sync code and trailing bytes are valid so that only the
    // show_existing_frame flag can reject it.
    Array<u8, 12> show_existing { 0x88, 0x49, 0x83, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    EXPECT(!Media::Codecs::VP9::parse_frame_header(show_existing).has_value());

    // Anything whose frame marker is not 2 is not a VP9 frame at all.
    Array<u8, 4> not_vp9 { 0x42, 0x49, 0x83, 0x42 };
    EXPECT(!Media::Codecs::VP9::parse_frame_header(not_vp9).has_value());
}

TEST_CASE(av1_sequence_header_reads_a_keyframe_format)
{
    // The head of av1_in_webm.webm's keyframe: a temporal delimiter, then the sequence header OBU.
    Array<u8, 17> keyframe {
        0x12, 0x00, 0x0a, 0x0c, 0x02, 0x00, 0x00, 0x25, 0x66, 0x35, 0x5e, 0xf8, 0xd5, 0xf3, 0x00, 0x80, 0x32
    };

    auto sequence_header = Media::Codecs::AV1::parse_sequence_header(keyframe);
    EXPECT(sequence_header.has_value());
    EXPECT_EQ(sequence_header->max_frame_size, Gfx::IntSize(854, 480));
    EXPECT_EQ(sequence_header->parameters.profile, 0);
    EXPECT_EQ(sequence_header->parameters.level, 4);
    EXPECT_EQ(to_underlying(sequence_header->parameters.tier), to_underlying(Media::Codecs::AV1::Tier::Main));
    EXPECT_EQ(sequence_header->parameters.bit_depth, 8);
    EXPECT(sequence_header->parameters.optional_fields.subsampling.x());
    EXPECT(sequence_header->parameters.optional_fields.subsampling.y());
    EXPECT(!sequence_header->parameters.optional_fields.monochrome);
}

TEST_CASE(av1_sequence_header_is_absent_from_frames_that_carry_none)
{
    // A temporal delimiter followed by a frame OBU, which is every coded frame but the first of a sequence.
    Array<u8, 6> inter_frame { 0x12, 0x00, 0x32, 0x02, 0x10, 0x00 };
    EXPECT(!Media::Codecs::AV1::parse_sequence_header(inter_frame).has_value());
}
