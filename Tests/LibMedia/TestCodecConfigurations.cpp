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
#include <LibMedia/Codecs/NALUnit.h>
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

TEST_CASE(h264_parameter_store_preserves_ids_and_replaces_sequence_definitions)
{
    Media::Codecs::H264::ParameterSetStore store;
    EXPECT(store.sequence_parameter_sets().is_empty());
    EXPECT_EQ(store.sequence_parameter_set(31), nullptr);
    EXPECT_EQ(store.sequence_parameter_set(255), nullptr);

    // Insert out of ID order and exceed inline capacity. All three SPSs infer a depth of five.
    Array<u8, 10> sps31 { 0x67, 0x42, 0x00, 0x1e, 0x04, 0x17, 0x03, 0x60, 0xf6, 0x40 };
    Array<u8, 8> sps0 { 0x67, 0x42, 0x00, 0x1e, 0xdc, 0x0d, 0x83, 0xd9 };
    Array<u8, 9> sps1 { 0x67, 0x42, 0x00, 0x1e, 0x57, 0x03, 0x60, 0xf6, 0x40 };
    EXPECT(MUST(store.apply_nal_unit(sps31)));
    EXPECT(MUST(store.apply_nal_unit(sps0)));
    EXPECT(MUST(store.apply_nal_unit(sps1)));
    EXPECT_EQ(store.sequence_parameter_sets().size(), 3u);
    for (u8 id : { 31, 0, 1 }) {
        auto const* set = store.sequence_parameter_set(id);
        VERIFY(set);
        EXPECT_EQ(set->parameters.seq_parameter_set_id, id);
        EXPECT_EQ(set->parameters.max_num_reorder_frames, 5);
    }
    EXPECT_EQ(store.sequence_parameter_set(2), nullptr);

    sps31[3] = 31; // Level 3.1 raises the inferred depth to eleven.
    EXPECT(MUST(store.apply_nal_unit(sps31)));
    EXPECT(!MUST(store.apply_nal_unit(sps31)));
    EXPECT_EQ(store.sequence_parameter_sets().size(), 3u);
    EXPECT_EQ(store.sequence_parameter_set(31)->parameters.max_num_reorder_frames, 11);
    EXPECT_EQ(store.sequence_parameter_set(0)->parameters.max_num_reorder_frames, 5);
    EXPECT_EQ(store.sequence_parameter_set(31)->nal_unit.bytes(), sps31.span());
    sps31[3] = 30;
    EXPECT_EQ(store.sequence_parameter_set(31)->nal_unit[3], 31); // Owned bytes.

    EXPECT(store.apply_nal_unit(sps31.span().trim(6)).is_error());
    EXPECT_EQ(store.sequence_parameter_set(31)->parameters.max_num_reorder_frames, 11);
    Array<u8, 2> slice { 0x65, 0x80 };
    EXPECT(!MUST(store.apply_nal_unit(slice)));
    EXPECT(store.apply_nal_unit({}).is_error());
    EXPECT_EQ(store.sequence_parameter_sets().size(), 3u);
}

TEST_CASE(h264_parameter_store_supports_every_picture_id_through_vector_growth)
{
    // Generate just the PPS prefix consumed by the parser: two ue(v) IDs (H.264, 7.3.2.2).
    auto pps_prefix = [](u32 picture_id, u32 sequence_id) {
        Array<u8, 5> nal_unit { 0x68, 0, 0, 0, 0 };
        size_t bit_position = 8;
        for (u32 value : { picture_id, sequence_id }) {
            u32 code = value + 1;
            size_t width = 0;
            for (auto remaining = code; remaining; remaining >>= 1)
                ++width;
            bit_position += width - 1;
            for (size_t bit = width; bit > 0; --bit, ++bit_position)
                nal_unit[bit_position / 8] |= ((code >> (bit - 1)) & 1) << (7 - bit_position % 8);
        }
        return nal_unit;
    };

    Media::Codecs::H264::ParameterSetStore store;
    // Multiplication by an odd number permutes all 256 IDs, preventing index == ID from masking bugs.
    for (size_t index = 0; index < 256; ++index) {
        auto id = static_cast<u8>(index * 73);
        EXPECT_EQ(store.picture_parameter_set(id), nullptr);
        auto nal_unit = pps_prefix(id, 31);
        EXPECT(MUST(store.apply_nal_unit(nal_unit)));
    }
    EXPECT_EQ(store.picture_parameter_sets().size(), 256u);
    for (size_t id = 0; id < 256; ++id) {
        auto const* set = store.picture_parameter_set(static_cast<u8>(id));
        VERIFY(set);
        EXPECT_EQ(set->parameters.pic_parameter_set_id, id);
        EXPECT_EQ(set->parameters.seq_parameter_set_id, 31);
    }
    auto replacement = pps_prefix(255, 0);
    EXPECT(MUST(store.apply_nal_unit(replacement)));
    EXPECT_EQ(store.picture_parameter_sets().size(), 256u);
    EXPECT_EQ(store.picture_parameter_set(255)->parameters.seq_parameter_set_id, 0);
    EXPECT_EQ(store.picture_parameter_set(254)->parameters.seq_parameter_set_id, 31);
    auto invalid = pps_prefix(255, 32);
    EXPECT(store.apply_nal_unit(invalid).is_error());
    EXPECT_EQ(store.picture_parameter_set(255)->parameters.seq_parameter_set_id, 0);
}

TEST_CASE(nal_unit_iterator_distinguishes_end_from_invalid_lengths)
{
    for (u8 length_size : { 1, 2, 3, 4 }) {
        Array<u8, 6> data { 0, 0, 0, 2, 0x67, 0x80 };
        auto framed = data.span().slice(4 - length_size);
        Media::Codecs::NALUnitIterator iterator { framed, length_size };
        auto nal_unit = iterator.next();
        EXPECT(nal_unit.has_value());
        if (nal_unit.has_value())
            EXPECT_EQ(*nal_unit, data.span().slice(4));
        EXPECT(!iterator.next().has_value());
        EXPECT(!iterator.has_error());

        Media::Codecs::NALUnitIterator truncated { framed.trim(framed.size() - 1), length_size };
        EXPECT(!truncated.next().has_value());
        EXPECT(truncated.has_error());
    }
    Array<u8, 1> trailing_length_byte { 0 };
    Media::Codecs::NALUnitIterator trailing { trailing_length_byte, 4 };
    EXPECT(!trailing.next().has_value());
    EXPECT(trailing.has_error());
    Media::Codecs::NALUnitIterator zero_length { trailing_length_byte, 1 };
    EXPECT(!zero_length.next().has_value());
    EXPECT(zero_length.has_error());
    for (u8 length_size : { 0, 5 }) {
        Media::Codecs::NALUnitIterator invalid { {}, length_size };
        EXPECT(!invalid.next().has_value());
        EXPECT(invalid.has_error());
    }
    Media::Codecs::NALUnitIterator empty { {}, 4 };
    EXPECT(!empty.next().has_value());
    EXPECT(!empty.has_error());
}

TEST_CASE(h264_configuration_supplies_both_sequence_and_picture_sets)
{
    // One sequence parameter set of 30 bytes, then one picture parameter set of 6, then trailing bytes.
    Array<u8, 51> record {
        0x01, 0x64, 0x00, 0x1e, 0xff, 0xe1, 0x00, 0x1e, 0x67, 0x64, 0x00, 0x1e, 0xac, 0xd9, 0x40, 0xd8,
        0x3d, 0xe6, 0xff, 0xf0, 0x35, 0x50, 0x35, 0x61, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03,
        0x00, 0x32, 0x0f, 0x16, 0x2d, 0x96, 0x01, 0x00, 0x06, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0xfd,
        0xf8, 0xf8, 0x00
    };

    auto sets = Media::Codecs::H264::parse_parameter_sets_from_configuration_record(record);
    EXPECT(sets.has_value());
    if (!sets.has_value())
        return;
    EXPECT_EQ(sets->nal_unit_length_size, 4);
    EXPECT_EQ(sets->sequence.size(), 1u);
    EXPECT_EQ(sets->picture.size(), 1u);
    if (sets->sequence.size() == 1)
        EXPECT_EQ(sets->sequence[0].size(), 30u);
    if (sets->picture.size() == 1)
        EXPECT_EQ(sets->picture[0].size(), 6u);
}

TEST_CASE(h264_configuration_rejects_a_three_byte_nal_unit_length_prefix)
{
    Array<u8, 10> record { 1, 0x64, 0, 0x1e, 0xff, 0xe0, 1, 0, 1, 0x68 };
    EXPECT(Media::Codecs::H264::parse_parameter_sets_from_configuration_record(record).has_value());

    // lengthSizeMinusOne of 2, which no configuration record may state.
    record[4] = 0xfe;
    EXPECT(!Media::Codecs::H264::parse_parameter_sets_from_configuration_record(record).has_value());
}

TEST_CASE(h264_representative_records_match_the_profiles_they_stand_in_for)
{
    for (u8 profile_idc : { 66, 77, 100, 110, 122, 244 }) {
        auto profile = Media::Codecs::H264::Parameters { .profile_idc = profile_idc, .constraint_set_flags = 0, .level_idc = 0 }.profile();
        EXPECT(profile.has_value());
        if (!profile.has_value())
            continue;
        auto record = Media::Codecs::H264::representative_configuration_record_for_profile(*profile);
        EXPECT(!record.is_empty());
        if (record.is_empty())
            continue;

        // The record has to describe the profile it is filed under, or it would answer for the wrong one.
        auto parameters = Media::Codecs::H264::parse_configuration_record(record);
        EXPECT(parameters.has_value());
        if (parameters.has_value())
            EXPECT_EQ(parameters->profile_idc, profile_idc);

        auto sets = Media::Codecs::H264::parse_parameter_sets_from_configuration_record(record);
        EXPECT(sets.has_value());
        if (!sets.has_value())
            continue;
        EXPECT_EQ(sets->sequence.size(), 1u);
        EXPECT_EQ(sets->picture.size(), 1u);
        EXPECT_EQ(sets->nal_unit_length_size, 4);

        // A stored set that will not parse cannot configure a decoder either.
        if (sets->sequence.size() == 1)
            EXPECT(Media::Codecs::H264::parse_sequence_parameter_set(sets->sequence[0]).has_value());
        if (sets->picture.size() == 1)
            EXPECT(Media::Codecs::H264::parse_picture_parameter_set(sets->picture[0]).has_value());
    }

    // Profiles no record was captured for, and that state no conformance to one, are left unanswerable.
    for (u8 profile_idc : { 0, 88, 44, 118, 128 })
        EXPECT(!(Media::Codecs::H264::Parameters { .profile_idc = profile_idc, .constraint_set_flags = 0, .level_idc = 0 }.profile().has_value()));
}

TEST_CASE(h264_profiles_follow_the_constraints_a_stream_states_conformance_to)
{
    using Profile = Media::Codecs::H264::Profile;
    static constexpr u8 CONSTRAINT_SET0_FLAG = 0x80;
    static constexpr u8 CONSTRAINT_SET1_FLAG = 0x40;
    static constexpr u8 CONSTRAINT_SET2_FLAG = 0x20;

    auto profile_of = [](u8 profile_idc, u8 constraint_set_flags) {
        return Media::Codecs::H264::Parameters { .profile_idc = profile_idc, .constraint_set_flags = constraint_set_flags, .level_idc = 30 }.profile();
    };
    auto expect_profile = [&](u8 profile_idc, u8 constraint_set_flags, Profile expected) {
        auto profile = profile_of(profile_idc, constraint_set_flags);
        EXPECT(profile.has_value());
        if (profile.has_value())
            EXPECT_EQ(to_underlying(*profile), to_underlying(expected));
    };

    // ITU-T H.264 (08/2024), 7.4.2.1.1: the Extended profile stating that it obeys the Baseline constraints, which
    // is what "avc1.58A01E" describes, needs only a Baseline decoder.
    expect_profile(88, CONSTRAINT_SET0_FLAG | CONSTRAINT_SET2_FLAG, Profile::Baseline);
    expect_profile(88, CONSTRAINT_SET1_FLAG, Profile::Main);

    // Conformance to the Extended profile alone leaves nothing to answer with.
    EXPECT(!profile_of(88, CONSTRAINT_SET2_FLAG).has_value());

    // The most constrained profile stated wins, even when the named one is known.
    expect_profile(100, CONSTRAINT_SET0_FLAG, Profile::Baseline);
    expect_profile(244, CONSTRAINT_SET1_FLAG, Profile::Main);
    expect_profile(100, 0, Profile::High);
}

TEST_CASE(h264_canonical_parameters_resolve_back_to_their_profile)
{
    using Profile = Media::Codecs::H264::Profile;
    for (auto profile : { Profile::Baseline, Profile::Main, Profile::High, Profile::High10, Profile::High422, Profile::High444 }) {
        auto parameters = Media::Codecs::H264::canonical_parameters_for_profile(profile);
        auto resolved = parameters.profile();
        EXPECT(resolved.has_value());
        if (resolved.has_value())
            EXPECT_EQ(to_underlying(*resolved), to_underlying(profile));
        EXPECT(!Media::Codecs::H264::representative_configuration_record_for_profile(profile).is_empty());
    }
}

TEST_CASE(h264_configuration_can_supply_only_picture_sets_or_no_sets)
{
    Array<u8, 10> picture_only { 1, 0x64, 0, 0x1e, 0xff, 0xe0, 1, 0, 1, 0x68 };
    auto sets = Media::Codecs::H264::parse_parameter_sets_from_configuration_record(picture_only);
    EXPECT(sets.has_value());
    if (sets.has_value()) {
        EXPECT(sets->sequence.is_empty());
        EXPECT_EQ(sets->picture.size(), 1u);
    }
    Array<u8, 7> no_sets { 1, 0x64, 0, 0x1e, 0xff, 0xe0, 0 };
    sets = Media::Codecs::H264::parse_parameter_sets_from_configuration_record(no_sets);
    EXPECT(sets.has_value());
    if (sets.has_value()) {
        EXPECT(sets->sequence.is_empty());
        EXPECT(sets->picture.is_empty());
    }
}

TEST_CASE(h264_sequence_parameter_set_returns_its_id_and_inferred_reorder_depth)
{
    // SPS ID 31, Baseline profile, level 3.0, 54 by 30 macroblocks, no VUI.
    Array<u8, 10> nal_unit { 0x67, 0x42, 0x00, 0x1e, 0x04, 0x17, 0x03, 0x60, 0xf6, 0x40 };
    auto parameter_set = Media::Codecs::H264::parse_sequence_parameter_set(nal_unit);
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->seq_parameter_set_id, 31);
    EXPECT_EQ(parameter_set->max_num_reorder_frames, 5);

    auto invalid_id = nal_unit;
    invalid_id[5] = 0x37; // seq_parameter_set_id = 32
    EXPECT(!Media::Codecs::H264::parse_sequence_parameter_set(invalid_id).has_value());
    EXPECT(!Media::Codecs::H264::parse_sequence_parameter_set(nal_unit.span().trim(5)).has_value());
    EXPECT(!Media::Codecs::H264::parse_sequence_parameter_set(nal_unit.span().trim(0)).has_value());
    nal_unit[0] = 0x68; // PPS NAL unit type
    EXPECT(!Media::Codecs::H264::parse_sequence_parameter_set(nal_unit).has_value());
    nal_unit[0] = 0xe7; // forbidden_zero_bit
    EXPECT(!Media::Codecs::H264::parse_sequence_parameter_set(nal_unit).has_value());
}

TEST_CASE(h264_picture_parameter_set_returns_its_id_and_sequence_reference)
{
    // PPS ID 255 referring to SPS ID 31, the upper bounds from H.264 (08/2024), 7.4.2.2.
    Array<u8, 7> nal_unit { 0x68, 0x00, 0x80, 0x02, 0x03, 0x8f, 0x20 };
    auto parameter_set = Media::Codecs::H264::parse_picture_parameter_set(nal_unit);
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->pic_parameter_set_id, 255);
    EXPECT_EQ(parameter_set->seq_parameter_set_id, 31);

    auto invalid_id = nal_unit;
    invalid_id[3] = 0x82; // pic_parameter_set_id = 256
    EXPECT(!Media::Codecs::H264::parse_picture_parameter_set(invalid_id).has_value());
    auto invalid_reference = nal_unit;
    invalid_reference[4] = 0x13; // seq_parameter_set_id = 32
    EXPECT(!Media::Codecs::H264::parse_picture_parameter_set(invalid_reference).has_value());
    EXPECT(!Media::Codecs::H264::parse_picture_parameter_set(nal_unit.span().trim(4)).has_value());
    EXPECT(!Media::Codecs::H264::parse_picture_parameter_set(nal_unit.span().trim(0)).has_value());
    nal_unit[0] = 0x67; // SPS NAL unit type
    EXPECT(!Media::Codecs::H264::parse_picture_parameter_set(nal_unit).has_value());
    nal_unit[0] = 0xe8; // forbidden_zero_bit
    EXPECT(!Media::Codecs::H264::parse_picture_parameter_set(nal_unit).has_value());
}

static Optional<u8> parsed_max_num_reorder_frames(ReadonlyBytes nal_unit)
{
    auto parameter_set = Media::Codecs::H264::parse_sequence_parameter_set(nal_unit);
    if (!parameter_set.has_value())
        return {};
    return parameter_set->max_num_reorder_frames;
}

TEST_CASE(h264_max_num_reorder_frames_comes_from_the_bitstream_restriction)
{
    // The High profile sequence parameter set from avc.mp4, which states two reorder frames. Its 0x000003
    // sequences are emulation prevention bytes, so the parse only lands correctly if they are dropped.
    Array<u8, 30> nal_unit {
        0x67, 0x64, 0x00, 0x1e, 0xac, 0xd9, 0x40, 0xd8, 0x3d, 0xe6, 0xff, 0xf0, 0x35, 0x50, 0x35, 0x61,
        0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x32, 0x0f, 0x16, 0x2d, 0x96
    };

    EXPECT_EQ(parsed_max_num_reorder_frames(nal_unit), 2);
}

TEST_CASE(h264_max_num_reorder_frames_reads_a_main_profile_sequence_parameter_set)
{
    // Main profile omits the chroma format and scaling matrix fields that High profile carries.
    Array<u8, 25> nal_unit {
        0x67, 0x4d, 0x40, 0x0a, 0xec, 0xa1, 0x42, 0x3f, 0x2e, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0x20,
        0x00, 0x00, 0x03, 0x02, 0x81, 0xe2, 0x44, 0xb2, 0xc0
    };

    EXPECT_EQ(parsed_max_num_reorder_frames(nal_unit), 2);
}

TEST_CASE(h264_configuration_rejects_a_parameter_set_running_past_the_record)
{
    Array<u8, 51> record {
        0x01, 0x64, 0x00, 0x1e, 0xff, 0xe1, 0x00, 0x1e, 0x67, 0x64, 0x00, 0x1e, 0xac, 0xd9, 0x40, 0xd8,
        0x3d, 0xe6, 0xff, 0xf0, 0x35, 0x50, 0x35, 0x61, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03,
        0x00, 0x32, 0x0f, 0x16, 0x2d, 0x96, 0x01, 0x00, 0x06, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0xfd,
        0xf8, 0xf8, 0x00
    };

    EXPECT(Media::Codecs::H264::parse_parameter_sets_from_configuration_record(record).has_value());
    EXPECT(!Media::Codecs::H264::parse_parameter_sets_from_configuration_record(record.span().trim(12)).has_value());
}

TEST_CASE(h264_max_num_reorder_frames_falls_back_to_the_level_when_unstated)
{
    // A Baseline profile sequence parameter set with no video usability information, 54 by 30 macroblocks at
    // level 3.0. The level allows 8100 macroblocks of decoded picture buffer, so 8100 / 1620 frames may reorder.
    Array<u8, 9> nal_unit { 0x67, 0x42, 0x00, 0x1e, 0xda, 0x03, 0x60, 0xf6, 0x40 };

    EXPECT_EQ(parsed_max_num_reorder_frames(nal_unit), 5);
}

TEST_CASE(h264_max_num_reorder_frames_infers_zero_from_profile_constraints)
{
    // ITU-T H.264 (08/2024), E.2.1: With no stated max_num_reorder_frames, constraint_set3_flag
    // implies zero only for the listed profiles. These SPSs describe 54 by 30 macroblocks at level 3.0,
    // so the otherwise inferred MaxDpbFrames is 5. Exercise both ways the restriction can be absent.
    Array<u8, 9> without_vui { 0x67, 0x64, 0x10, 0x1e, 0xac, 0xb8, 0x1b, 0x07, 0xb2 };
    Array<u8, 10> without_bitstream_restriction { 0x67, 0x64, 0x10, 0x1e, 0xac, 0xb8, 0x1b, 0x07, 0xb4, 0x01 };

    auto check_inference = [](auto nal_unit) {
        for (u8 profile_idc : { 44, 86, 100, 110, 122, 244 }) {
            nal_unit[1] = profile_idc;
            nal_unit[2] = 0x10; // constraint_set3_flag
            EXPECT_EQ(parsed_max_num_reorder_frames(nal_unit), 0);
            nal_unit[2] = 0;
            EXPECT_EQ(parsed_max_num_reorder_frames(nal_unit), 5);
        }

        // Profile 118 shares the parsed SPS syntax but is outside the list for this inference rule.
        nal_unit[1] = 118;
        nal_unit[2] = 0x10;
        EXPECT_EQ(parsed_max_num_reorder_frames(nal_unit), 5);
    };
    check_inference(without_vui);
    check_inference(without_bitstream_restriction);
}
