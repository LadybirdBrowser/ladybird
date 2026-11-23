/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Codecs/Opus.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/Codecs/AAC.h>
#include <LibMediaCodecs/Codecs/AV1.h>
#include <LibMediaCodecs/Codecs/DolbyVision.h>
#include <LibMediaCodecs/Codecs/H264.h>
#include <LibMediaCodecs/Codecs/H265.h>
#include <LibMediaCodecs/Codecs/HDR10Plus.h>
#include <LibMediaCodecs/Codecs/HighDynamicRangeMetadataTracker.h>
#include <LibMediaCodecs/Codecs/NALUnit.h>
#include <LibMediaCodecs/Codecs/VP9.h>
#include <LibMediaCodecs/Limits.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

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
    EXPECT_EQ(parameters->optional_fields.cicp.color_primaries(), Media::ColorPrimaries::BT2020);
    EXPECT_EQ(parameters->optional_fields.cicp.transfer_characteristics(), Media::TransferCharacteristics::SMPTE2084);
    EXPECT_EQ(parameters->optional_fields.cicp.matrix_coefficients(), Media::MatrixCoefficients::BT2020NonConstantLuminance);
    EXPECT_EQ(parameters->optional_fields.cicp.video_full_range_flag(), Media::VideoFullRangeFlag::Studio);
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
    EXPECT_EQ(parameters->optional_fields.cicp.color_primaries(), Media::ColorPrimaries::Unspecified);
    EXPECT_EQ(parameters->optional_fields.cicp.transfer_characteristics(), Media::TransferCharacteristics::Unspecified);
    EXPECT_EQ(parameters->optional_fields.cicp.matrix_coefficients(), Media::MatrixCoefficients::Unspecified);
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
    EXPECT_EQ(parameters->optional_fields.cicp.color_primaries(), Media::ColorPrimaries::Unspecified);
    EXPECT_EQ(parameters->optional_fields.cicp.transfer_characteristics(), Media::TransferCharacteristics::Unspecified);
    EXPECT_EQ(parameters->optional_fields.cicp.matrix_coefficients(), Media::MatrixCoefficients::Unspecified);
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

TEST_CASE(aac_explicit_spectral_band_replication_is_read_from_hierarchical_signaling)
{
    // SBR at 48000 Hz over an AAC-LC core at 48000 Hz, stereo, as hls.js 0.12.4 writes for Chromium.
    Array<u8, 4> spectral_band_replication { 0x29, 0x91, 0x88, 0x00 };
    auto parameters = Media::Codecs::AAC::parse_explicit_spectral_band_replication(spectral_band_replication);
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->core_audio_object_type, Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE);
    EXPECT_EQ(parameters->core_sample_rate, 48000u);
    EXPECT_EQ(parameters->channel_count, 2);
    EXPECT_EQ(parameters->general_audio_specific_config, Media::Codecs::AAC::GeneralAudioSpecificConfig {});
    EXPECT_EQ(parameters->output_sample_rate, 48000u);

    // Parametric stereo at 48000 Hz over a mono AAC-LC core at 24000 Hz.
    Array<u8, 4> parametric_stereo { 0xeb, 0x09, 0x88, 0x00 };
    parameters = Media::Codecs::AAC::parse_explicit_spectral_band_replication(parametric_stereo);
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->core_sample_rate, 24000u);
    EXPECT_EQ(parameters->channel_count, 1);
    EXPECT_EQ(parameters->output_sample_rate, 48000u);
}

TEST_CASE(aac_explicit_spectral_band_replication_is_read_from_backward_compatible_signaling)
{
    auto created = TRY_OR_FAIL(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 24000, 2, 48000));
    auto parameters = Media::Codecs::AAC::parse_explicit_spectral_band_replication(created.span());
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->core_audio_object_type, Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE);
    EXPECT_EQ(parameters->core_sample_rate, 24000u);
    EXPECT_EQ(parameters->channel_count, 2);
    EXPECT_EQ(parameters->general_audio_specific_config, Media::Codecs::AAC::GeneralAudioSpecificConfig {});
    EXPECT_EQ(parameters->output_sample_rate, 48000u);

    // A core coder delay lengthens the GASpecificConfig before the sync extension.
    Array<u8, 7> depends_on_core_coder { 0x13, 0x92, 0x91, 0xa1, 0x5b, 0x96, 0x80 };
    parameters = Media::Codecs::AAC::parse_explicit_spectral_band_replication(depends_on_core_coder);
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->core_sample_rate, 22050u);
    EXPECT_EQ(parameters->general_audio_specific_config.core_coder_delay, 0x1234);
    EXPECT_EQ(parameters->output_sample_rate, 44100u);

    Array<u8, 8> escaped_output_sample_rate { 0x11, 0x90, 0x56, 0xe5, 0xf8, 0x06, 0x1a, 0x80 };
    parameters = Media::Codecs::AAC::parse_explicit_spectral_band_replication(escaped_output_sample_rate);
    EXPECT(parameters.has_value());
    EXPECT_EQ(parameters->output_sample_rate, 50000u);
}

TEST_CASE(aac_explicit_spectral_band_replication_is_absent_or_not_understood)
{
    Array<u8, 2> low_complexity { 0x11, 0x90 };
    EXPECT(!Media::Codecs::AAC::parse_explicit_spectral_band_replication(low_complexity).has_value());

    // The sync extension declares that SBR is not present.
    Array<u8, 5> declared_absent { 0x11, 0x90, 0x56, 0xe5, 0x00 };
    EXPECT(!Media::Codecs::AAC::parse_explicit_spectral_band_replication(declared_absent).has_value());

    // A channel configuration of zero carries a program config element.
    Array<u8, 4> program_config_element { 0x29, 0x81, 0x88, 0x00 };
    EXPECT(!Media::Codecs::AAC::parse_explicit_spectral_band_replication(program_config_element).has_value());

    // An AAC-LD core.
    Array<u8, 4> low_delay_core { 0x29, 0x91, 0xdc, 0x00 };
    EXPECT(!Media::Codecs::AAC::parse_explicit_spectral_band_replication(low_delay_core).has_value());

    Array<u8, 3> truncated { 0x29, 0x91, 0x88 };
    EXPECT(!Media::Codecs::AAC::parse_explicit_spectral_band_replication(truncated).has_value());
}

static Optional<FixedArray<u8>> core_configuration_record(ReadonlyBytes audio_specific_config)
{
    auto parameters = Media::Codecs::AAC::parse_explicit_spectral_band_replication(audio_specific_config);
    if (!parameters.has_value())
        return {};
    return MUST(Media::Codecs::AAC::create_configuration_record(parameters->core_audio_object_type, parameters->core_sample_rate, parameters->channel_count, {}, parameters->general_audio_specific_config));
}

TEST_CASE(aac_configuration_record_is_recreated_from_its_core)
{
    Array<u8, 2> expected_low_complexity { 0x11, 0x90 };

    Array<u8, 4> hierarchical { 0x29, 0x91, 0x88, 0x00 };
    EXPECT_EQ(core_configuration_record(hierarchical)->span(), expected_low_complexity.span());

    auto backward_compatible = TRY_OR_FAIL(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 48000, 2, 48000));
    EXPECT_EQ(core_configuration_record(backward_compatible.span())->span(), expected_low_complexity.span());

    // The GASpecificConfig is carried over: 960-sample frames, a core coder delay, and the third extension flag.
    Array<u8, 4> frame_length_flag { 0x29, 0x91, 0x8a, 0x00 };
    Array<u8, 2> expected_frame_length_flag { 0x11, 0x94 };
    EXPECT_EQ(core_configuration_record(frame_length_flag)->span(), expected_frame_length_flag.span());

    Array<u8, 7> depends_on_core_coder { 0x13, 0x92, 0x91, 0xa1, 0x5b, 0x96, 0x80 };
    Array<u8, 4> expected_depends_on_core_coder { 0x13, 0x92, 0x91, 0xa0 };
    EXPECT_EQ(core_configuration_record(depends_on_core_coder)->span(), expected_depends_on_core_coder.span());

    Array<u8, 4> extension_flag_3 { 0x29, 0x91, 0x88, 0xc0 };
    Array<u8, 3> expected_extension_flag_3 { 0x11, 0x91, 0x80 };
    EXPECT_EQ(core_configuration_record(extension_flag_3)->span(), expected_extension_flag_3.span());
}

TEST_CASE(aac_configuration_record_is_created_from_stream_parameters)
{
    auto low_complexity = TRY_OR_FAIL(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 48000, 2));
    Array<u8, 2> expected_low_complexity { 0x11, 0x90 };
    EXPECT_EQ(low_complexity.span(), expected_low_complexity.span());

    // A stream that upsamples names its extension's rate after the core configuration.
    auto spectral_band_replication = TRY_OR_FAIL(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 24000, 2, 48000));
    Array<u8, 5> expected_spectral_band_replication { 0x13, 0x10, 0x56, 0xe5, 0x98 };
    EXPECT_EQ(spectral_band_replication.span(), expected_spectral_band_replication.span());
}

TEST_CASE(aac_configuration_record_escapes_a_sample_rate_it_cannot_name)
{
    auto configuration = TRY_OR_FAIL(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 37800, 1));

    // The escaped index is followed by the rate itself, then the channel configuration.
    Array<u8, 5> expected { 0x17, 0x80, 0x49, 0xd4, 0x08 };
    EXPECT_EQ(configuration.span(), expected.span());
}

TEST_CASE(aac_configuration_record_rejects_channel_counts_it_cannot_describe)
{
    EXPECT(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 48000, 7).is_error());
    EXPECT(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 48000, 0).is_error());
}

TEST_CASE(aac_configuration_record_is_wrapped_in_an_elementary_stream_descriptor)
{
    // AAC-LC at 48000 Hz, stereo.
    Array<u8, 2> audio_specific_config { 0x11, 0x90 };

    auto descriptor = TRY_OR_FAIL(Media::Codecs::AAC::elementary_stream_descriptor_for_configuration_record(audio_specific_config));

    Array<u8, 39> expected {
        0x03, 0x80, 0x80, 0x80, 0x22,
        0x00, 0x00, 0x00,
        0x04, 0x80, 0x80, 0x80, 0x14,
        0x40, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x05, 0x80, 0x80, 0x80, 0x02,
        0x11, 0x90,
        0x06, 0x80, 0x80, 0x80, 0x01,
        0x02
    };
    EXPECT_EQ(descriptor.span(), expected.span());
}

TEST_CASE(aac_elementary_stream_descriptor_omits_an_empty_configuration_record)
{
    auto descriptor = TRY_OR_FAIL(Media::Codecs::AAC::elementary_stream_descriptor_for_configuration_record({}));

    Array<u8, 32> expected {
        0x03, 0x80, 0x80, 0x80, 0x1b,
        0x00, 0x00, 0x00,
        0x04, 0x80, 0x80, 0x80, 0x0d,
        0x40, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x06, 0x80, 0x80, 0x80, 0x01,
        0x02
    };
    EXPECT_EQ(descriptor.span(), expected.span());
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

// The video, sequence and picture parameter sets from the hvcC record of an HEVC Main profile file. The sequence
// set's 0x000003 sequences are emulation prevention bytes, so its ids only land correctly if they are dropped.
static constexpr Array<u8, 24> hevc_video_parameter_set {
    0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x3f, 0x95, 0x98, 0x09
};
static constexpr Array<u8, 42> hevc_sequence_parameter_set {
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x3f, 0xa0, 0x05, 0x02, 0x01, 0x69, 0x65, 0x95, 0x9a, 0x49, 0x32, 0xbc, 0x04, 0x04, 0x00,
    0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x78, 0x20
};
static constexpr Array<u8, 7> hevc_picture_parameter_set { 0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40 };

static Vector<u8> hevc_configuration_record_from(ReadonlyBytes video, ReadonlyBytes sequence, ReadonlyBytes picture)
{
    Array<u8, 23> fixed_header {};
    fixed_header[0] = 1;     // configurationVersion
    fixed_header[21] = 0xf3; // lengthSizeMinusOne of 3, so a four-byte prefix
    fixed_header[22] = 3;    // numOfArrays

    Vector<u8> record;
    record.append(fixed_header.data(), fixed_header.size());

    auto append_array = [&](u8 nal_unit_type, ReadonlyBytes nal_unit) {
        record.append(0x80 | nal_unit_type); // array_completeness
        record.append(0);
        record.append(1); // numNalus
        record.append(static_cast<u8>(nal_unit.size() >> 8));
        record.append(static_cast<u8>(nal_unit.size()));
        record.append(nal_unit.data(), nal_unit.size());
    };
    append_array(32, video);
    append_array(33, sequence);
    append_array(34, picture);
    return record;
}

TEST_CASE(h265_nal_unit_header_spans_two_bytes)
{
    auto header = Media::Codecs::H265::parse_nal_unit_header(hevc_sequence_parameter_set);
    EXPECT(header.has_value());
    if (!header.has_value())
        return;
    EXPECT_EQ(header->nal_unit_type, 33);
    EXPECT_EQ(header->nuh_layer_id, 0);
    EXPECT_EQ(header->temporal_id, 0);
    EXPECT(!Media::Codecs::H265::is_coded_slice(*header));

    // The type occupies the six bits below the forbidden zero bit, so H.264's mask would read 2 rather than 33.
    EXPECT_EQ(hevc_sequence_parameter_set[0] & 0x1f, 2);

    auto nal_unit = hevc_sequence_parameter_set;
    EXPECT(!Media::Codecs::H265::parse_nal_unit_header(nal_unit.span().trim(1)).has_value());
    nal_unit[0] |= 0x80; // forbidden_zero_bit
    EXPECT(!Media::Codecs::H265::parse_nal_unit_header(nal_unit).has_value());
    nal_unit = hevc_sequence_parameter_set;
    nal_unit[1] &= ~0x07; // nuh_temporal_id_plus1 of zero
    EXPECT(!Media::Codecs::H265::parse_nal_unit_header(nal_unit).has_value());
}

TEST_CASE(h265_sequence_parameter_set_returns_its_ids_and_reorder_depth)
{
    auto parameter_set = Media::Codecs::H265::parse_sequence_parameter_set(hevc_sequence_parameter_set);
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->sps_seq_parameter_set_id, 0);
    EXPECT_EQ(parameter_set->sps_video_parameter_set_id, 0);
    EXPECT_EQ(parameter_set->sps_max_num_reorder_pics, 2);

    EXPECT(!Media::Codecs::H265::parse_sequence_parameter_set(hevc_video_parameter_set).has_value());
    EXPECT(!Media::Codecs::H265::parse_sequence_parameter_set(hevc_sequence_parameter_set.span().trim(6)).has_value());
}

// x265's sequence parameter set for a 4:4:4 RGB stream, which states an identity matrix in its VUI.
static constexpr Array<u8, 43> hevc_rgb_sequence_parameter_set {
    0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9e, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x3c, 0x90, 0x01, 0x41, 0x01, 0xe2, 0xcb, 0x2b, 0x34, 0x92, 0x65, 0x78, 0x0b, 0x70, 0x20, 0x20,
    0x00, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x06, 0x42
};

// A 4:4:4 sequence parameter set whose VUI follows scaling lists, PCM, three short-term reference picture sets (the
// second predicted from the first) and two long-term reference pictures. It states BT.709 primaries, the sRGB transfer
// and an identity matrix in full range.
static constexpr Array<u8, 76> hevc_sequence_parameter_set_with_reference_picture_sets {
    0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x5d, 0x90, 0x01, 0x41, 0x01, 0xe2, 0xcb, 0x2b, 0xc9, 0x27, 0xaa, 0xaa, 0xaa, 0xed, 0x34,
    0xd3, 0x4d, 0x34, 0xd3, 0x4d, 0x34, 0xd3, 0x4d, 0x34, 0xd3, 0x4d, 0x34, 0xd3, 0x4d, 0x34, 0xd3,
    0x4d, 0x34, 0xd3, 0x4d, 0x34, 0xd3, 0x4a, 0xaa, 0xf7, 0x7c, 0x46, 0xbf, 0xf6, 0x15, 0xec, 0x16,
    0x12, 0xff, 0xf0, 0x00, 0x10, 0x00, 0x1b, 0x70, 0x10, 0xd0, 0x00, 0x08
};

TEST_CASE(h265_sequence_parameter_set_reads_its_chroma_format_and_colour_description)
{
    auto parameter_set = Media::Codecs::H265::parse_sequence_parameter_set(hevc_rgb_sequence_parameter_set);
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->chroma_format_idc, 3);
    EXPECT_EQ(parameter_set->cicp.matrix_coefficients(), Media::MatrixCoefficients::Identity);
    EXPECT_EQ(parameter_set->cicp.video_full_range_flag(), Media::VideoFullRangeFlag::Full);

    parameter_set = Media::Codecs::H265::parse_sequence_parameter_set(hevc_sequence_parameter_set);
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->chroma_format_idc, 1);
}

TEST_CASE(h265_sequence_parameter_set_reads_the_colour_description_past_reference_picture_sets)
{
    auto parameter_set = Media::Codecs::H265::parse_sequence_parameter_set(hevc_sequence_parameter_set_with_reference_picture_sets);
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->chroma_format_idc, 3);
    EXPECT_EQ(parameter_set->sps_max_num_reorder_pics, 2);
    EXPECT_EQ(parameter_set->cicp.color_primaries(), Media::ColorPrimaries::BT709);
    EXPECT_EQ(parameter_set->cicp.transfer_characteristics(), Media::TransferCharacteristics::SRGB);
    EXPECT_EQ(parameter_set->cicp.matrix_coefficients(), Media::MatrixCoefficients::Identity);
    EXPECT_EQ(parameter_set->cicp.video_full_range_flag(), Media::VideoFullRangeFlag::Full);
}

TEST_CASE(h265_sequence_parameter_set_without_a_readable_colour_description_reports_unspecified_color)
{
    // Cut well before the VUI, but after every field a decoder is configured from.
    auto parameter_set = Media::Codecs::H265::parse_sequence_parameter_set(hevc_sequence_parameter_set_with_reference_picture_sets.span().trim(56));
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->chroma_format_idc, 3);
    EXPECT_EQ(parameter_set->cicp, Media::CodingIndependentCodePoints {});
}

// x264's sequence parameter set for a High 4:4:4 Predictive RGB stream, which states an identity matrix in its VUI.
static constexpr Array<u8, 29> avc_rgb_sequence_parameter_set {
    0x67, 0xf4, 0x00, 0x0d, 0x91, 0x9b, 0x28, 0x28, 0x3f, 0x60, 0x2d, 0xc0, 0x80, 0x80, 0x10, 0x00,
    0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x20, 0xf1, 0x42, 0x99, 0x60
};

TEST_CASE(h264_sequence_parameter_set_reads_its_chroma_format_and_colour_description)
{
    auto parameter_set = Media::Codecs::H264::parse_sequence_parameter_set(avc_rgb_sequence_parameter_set);
    EXPECT(parameter_set.has_value());
    if (!parameter_set.has_value())
        return;
    EXPECT_EQ(parameter_set->chroma_format_idc, 3);
    EXPECT_EQ(parameter_set->bit_depth_luma, 8);
    EXPECT_EQ(parameter_set->cicp.matrix_coefficients(), Media::MatrixCoefficients::Identity);
    EXPECT_EQ(parameter_set->cicp.video_full_range_flag(), Media::VideoFullRangeFlag::Full);
}

TEST_CASE(h265_video_and_picture_parameter_sets_return_their_ids)
{
    auto video_parameter_set = Media::Codecs::H265::parse_video_parameter_set(hevc_video_parameter_set);
    EXPECT(video_parameter_set.has_value());
    if (video_parameter_set.has_value())
        EXPECT_EQ(video_parameter_set->vps_video_parameter_set_id, 0);

    auto picture_parameter_set = Media::Codecs::H265::parse_picture_parameter_set(hevc_picture_parameter_set);
    EXPECT(picture_parameter_set.has_value());
    if (picture_parameter_set.has_value()) {
        EXPECT_EQ(picture_parameter_set->pps_pic_parameter_set_id, 0);
        EXPECT_EQ(picture_parameter_set->pps_seq_parameter_set_id, 0);
    }

    EXPECT(!Media::Codecs::H265::parse_video_parameter_set(hevc_picture_parameter_set).has_value());
    EXPECT(!Media::Codecs::H265::parse_picture_parameter_set(hevc_video_parameter_set).has_value());
}

TEST_CASE(h265_representative_records_match_the_profiles_they_stand_in_for)
{
    struct ProfileCase {
        u8 profile_idc;
        Array<u8, 6> constraint_indicator_flags;
        u8 bit_depth;
    };
    // The range extensions profiles share profile_idc 4 and are told apart by their constraint flags.
    Array<ProfileCase, 8> cases { {
        { 1, { 0x90, 0, 0, 0, 0, 0 }, 8 },
        { 2, { 0x90, 0, 0, 0, 0, 0 }, 10 },
        { 4, { 0x99, 0x88, 0, 0, 0, 0 }, 12 },
        { 4, { 0x9d, 0x08, 0, 0, 0, 0 }, 10 },
        { 4, { 0x99, 0x08, 0, 0, 0, 0 }, 12 },
        { 4, { 0x9e, 0x08, 0, 0, 0, 0 }, 8 },
        { 4, { 0x9c, 0x08, 0, 0, 0, 0 }, 10 },
        { 4, { 0x98, 0x08, 0, 0, 0, 0 }, 12 },
    } };

    for (auto const& profile_case : cases) {
        Media::Codecs::H265::Parameters parameters {};
        parameters.profile_idc = profile_case.profile_idc;
        parameters.constraint_indicator_flags = profile_case.constraint_indicator_flags;
        auto profile = parameters.profile();
        EXPECT(profile.has_value());
        if (!profile.has_value())
            continue;

        auto record = Media::Codecs::H265::representative_configuration_record_for_profile(*profile);
        EXPECT(!record.is_empty());

        // The record has to describe the profile it is filed under, or it would answer for the wrong one.
        auto record_parameters = Media::Codecs::H265::parse_configuration_record(record);
        EXPECT(record_parameters.has_value());
        if (record_parameters.has_value()) {
            EXPECT_EQ(record_parameters->profile_idc, profile_case.profile_idc);
            EXPECT_EQ(record_parameters->constraint_indicator_flags, profile_case.constraint_indicator_flags);
        }

        auto sets = Media::Codecs::H265::parse_parameter_sets_from_configuration_record(record);
        EXPECT(sets.has_value());
        if (!sets.has_value())
            continue;
        EXPECT_EQ(sets->video.size(), 1u);
        EXPECT_EQ(sets->sequence.size(), 1u);
        EXPECT_EQ(sets->picture.size(), 1u);

        // The bit depth a profile allows is what a decoder judges, so the stored set has to carry it.
        if (sets->sequence.size() == 1) {
            auto sequence_set = Media::Codecs::H265::parse_sequence_parameter_set(sets->sequence[0]);
            EXPECT(sequence_set.has_value());
            if (sequence_set.has_value())
                EXPECT_EQ(sequence_set->bit_depth_luma, profile_case.bit_depth);
        }
    }
}

TEST_CASE(h265_profiles_cover_the_ones_that_only_constrain_others)
{
    using Profile = Media::Codecs::H265::Profile;
    auto profile_of = [](u8 profile_space, u8 profile_idc, Array<u8, 6> const& flags, u32 compatibility_flags = 0) {
        Media::Codecs::H265::Parameters parameters {};
        parameters.profile_space = profile_space;
        parameters.profile_idc = profile_idc;
        parameters.constraint_indicator_flags = flags;
        parameters.profile_compatibility_flags = compatibility_flags;
        return parameters.profile();
    };
    auto expect_profile = [&](Array<u8, 6> const& flags, Profile expected) {
        auto profile = profile_of(0, 4, flags);
        EXPECT(profile.has_value());
        if (profile.has_value())
            EXPECT_EQ(to_underlying(*profile), to_underlying(expected));
    };

    // A profile allowing more than the stream states covers it, so these resolve to the profile they constrain.
    expect_profile({ 0x9f, 0xa8, 0, 0, 0, 0 }, Profile::Main);    // Main Still Picture
    expect_profile({ 0x9d, 0xa8, 0, 0, 0, 0 }, Profile::Main10);  // Main 10 Intra
    expect_profile({ 0x9e, 0x28, 0, 0, 0, 0 }, Profile::Main444); // Main 4:4:4 Intra

    // ITU-T H.265 (07/2024), A.3.2: stated conformance to a tighter profile is what a decoder need only support,
    // whatever profile the stream names.
    auto expect_claimed_profile = [&](u8 profile_idc, Array<u8, 6> const& flags, u32 compatibility_flags, Profile expected) {
        auto profile = profile_of(0, profile_idc, flags, compatibility_flags);
        EXPECT(profile.has_value());
        if (profile.has_value())
            EXPECT_EQ(to_underlying(*profile), to_underlying(expected));
    };
    expect_claimed_profile(4, { 0x99, 0x88, 0, 0, 0, 0 }, 1u << 1, Profile::Main);
    expect_claimed_profile(4, { 0x98, 0x08, 0, 0, 0, 0 }, 1u << 2, Profile::Main10);
    // A Main 10 stream that is really 8-bit says so, and needs only a Main decoder.
    expect_claimed_profile(2, { 0x90, 0, 0, 0, 0, 0 }, (1u << 1) | (1u << 2), Profile::Main);
    // Naming Main 10 without claiming Main leaves it at Main 10.
    expect_claimed_profile(2, { 0x90, 0, 0, 0, 0, 0 }, 1u << 2, Profile::Main10);
    // Main decoders decode Main Still Picture streams, whether they name the profile or only claim it.
    expect_claimed_profile(3, { 0x90, 0, 0, 0, 0, 0 }, 0, Profile::Main);
    expect_claimed_profile(4, { 0x99, 0x88, 0, 0, 0, 0 }, 1u << 3, Profile::Main);

    // A profile space other than zero names a different set of profiles entirely.
    EXPECT(!profile_of(1, 1, { 0x90, 0, 0, 0, 0, 0 }).has_value());
    // Profiles no record was captured for are left unanswerable.
    for (u8 profile_idc : { 0, 5, 9, 11 })
        EXPECT(!profile_of(0, profile_idc, { 0x90, 0, 0, 0, 0, 0 }).has_value());
    // Monochrome and bit depths beyond 12 have no record to answer with.
    EXPECT(!profile_of(0, 4, { 0x9e, 0x40, 0, 0, 0, 0 }).has_value());
    EXPECT(!profile_of(0, 4, { 0x80, 0x08, 0, 0, 0, 0 }).has_value());
}

TEST_CASE(h265_canonical_parameters_resolve_back_to_their_profile)
{
    using Profile = Media::Codecs::H265::Profile;
    for (auto profile : { Profile::Main, Profile::Main10, Profile::Main12, Profile::Main422_10,
             Profile::Main422_12, Profile::Main444, Profile::Main444_10, Profile::Main444_12 }) {
        auto parameters = Media::Codecs::H265::canonical_parameters_for_profile(profile);
        auto resolved = parameters.profile();
        EXPECT(resolved.has_value());
        if (resolved.has_value())
            EXPECT_EQ(to_underlying(*resolved), to_underlying(profile));
        EXPECT(!Media::Codecs::H265::representative_configuration_record_for_profile(profile).is_empty());
    }
}

TEST_CASE(h265_configuration_supplies_every_parameter_set_type)
{
    auto record = hevc_configuration_record_from(hevc_video_parameter_set, hevc_sequence_parameter_set, hevc_picture_parameter_set);
    auto sets = Media::Codecs::H265::parse_parameter_sets_from_configuration_record(record);
    EXPECT(sets.has_value());
    if (!sets.has_value())
        return;
    EXPECT_EQ(sets->nal_unit_length_size, 4);
    EXPECT_EQ(sets->video.size(), 1u);
    EXPECT_EQ(sets->sequence.size(), 1u);
    EXPECT_EQ(sets->picture.size(), 1u);
    if (sets->sequence.size() == 1)
        EXPECT_EQ(sets->sequence[0].size(), hevc_sequence_parameter_set.size());
    if (sets->picture.size() == 1)
        EXPECT_EQ(sets->picture[0].size(), hevc_picture_parameter_set.size());
}

TEST_CASE(h265_configuration_rejects_records_it_cannot_walk)
{
    auto record = hevc_configuration_record_from(hevc_video_parameter_set, hevc_sequence_parameter_set, hevc_picture_parameter_set);
    EXPECT(Media::Codecs::H265::parse_parameter_sets_from_configuration_record(record).has_value());

    // Shorter than configurationVersion through numOfArrays.
    EXPECT(!Media::Codecs::H265::parse_parameter_sets_from_configuration_record(record.span().trim(22)).has_value());

    auto rejected = record;
    rejected[0] = 2; // configurationVersion
    EXPECT(!Media::Codecs::H265::parse_parameter_sets_from_configuration_record(rejected).has_value());

    rejected = record;
    rejected[21] = 0xf2; // lengthSizeMinusOne of 2
    EXPECT(!Media::Codecs::H265::parse_parameter_sets_from_configuration_record(rejected).has_value());

    // A record cut short of the sets it promises keeps what it managed to carry.
    auto truncated = Media::Codecs::H265::parse_parameter_sets_from_configuration_record(record.span().trim(record.size() - 20));
    EXPECT(truncated.has_value());
    if (truncated.has_value()) {
        EXPECT_EQ(truncated->video.size(), 1u);
        EXPECT_EQ(truncated->picture.size(), 0u);
    }
}

TEST_CASE(h265_parameter_store_keys_every_set_type_by_id)
{
    Media::Codecs::H265::ParameterSetStore store;
    EXPECT_EQ(MUST(store.apply_nal_unit(hevc_video_parameter_set)), true);
    EXPECT_EQ(MUST(store.apply_nal_unit(hevc_sequence_parameter_set)), true);
    EXPECT_EQ(MUST(store.apply_nal_unit(hevc_picture_parameter_set)), true);

    // A repeated definition is not a change.
    EXPECT_EQ(MUST(store.apply_nal_unit(hevc_sequence_parameter_set)), false);

    EXPECT_EQ(store.video_parameter_sets().size(), 1u);
    EXPECT_EQ(store.sequence_parameter_sets().size(), 1u);
    EXPECT_EQ(store.picture_parameter_sets().size(), 1u);
    EXPECT(store.video_parameter_set(0) != nullptr);
    EXPECT(store.sequence_parameter_set(0) != nullptr);
    EXPECT(store.picture_parameter_set(0) != nullptr);
    EXPECT(store.sequence_parameter_set(1) == nullptr);
    if (store.sequence_parameter_set(0) != nullptr)
        EXPECT_EQ(store.sequence_parameter_set(0)->parameters.sps_max_num_reorder_pics, 2);
}

TEST_CASE(h265_parameter_store_ignores_enhancement_layer_sets)
{
    Media::Codecs::H265::ParameterSetStore store;
    auto enhancement_layer_set = hevc_sequence_parameter_set;
    enhancement_layer_set[1] |= 0x08; // nuh_layer_id of one

    EXPECT_EQ(MUST(store.apply_nal_unit(enhancement_layer_set)), false);
    EXPECT_EQ(store.sequence_parameter_sets().size(), 0u);

    // The base layer's set of the same id is still accepted afterwards.
    EXPECT_EQ(MUST(store.apply_nal_unit(hevc_sequence_parameter_set)), true);
    EXPECT_EQ(store.sequence_parameter_sets().size(), 1u);
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
    for (u8 profile_idc : { 66, 77, 88, 100, 110, 122, 244, 44, 83, 86, 118, 128 }) {
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

    // Profiles no record exists for, and that state no conformance to one, are left unanswerable.
    for (u8 profile_idc : { 0, 134, 135, 138, 139 })
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

    // A.2.3: only an Extended decoder is required to accept streams flagged constraint_set2, so the flag never
    // narrows a named profile.
    expect_profile(88, CONSTRAINT_SET2_FLAG, Profile::Extended);
    expect_profile(100, CONSTRAINT_SET2_FLAG, Profile::High);
    expect_profile(77, CONSTRAINT_SET2_FLAG, Profile::Main);

    // The most constrained profile stated wins, even when the named one is known.
    expect_profile(100, CONSTRAINT_SET0_FLAG, Profile::Baseline);
    expect_profile(244, CONSTRAINT_SET1_FLAG, Profile::Main);
    expect_profile(128, CONSTRAINT_SET1_FLAG, Profile::Main);
    expect_profile(83, CONSTRAINT_SET0_FLAG, Profile::Baseline);
    expect_profile(100, 0, Profile::High);
}

TEST_CASE(h264_canonical_parameters_resolve_back_to_their_profile)
{
    using Profile = Media::Codecs::H264::Profile;
    for (auto profile : { Profile::Baseline, Profile::Main, Profile::Extended, Profile::High, Profile::High10, Profile::High422, Profile::High444, Profile::CAVLC444Intra, Profile::ScalableBaseline, Profile::ScalableHigh, Profile::MultiviewHigh, Profile::StereoHigh }) {
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

TEST_CASE(h264_sequence_parameter_set_reads_its_profile_chroma_format_and_bit_depth)
{
    auto expect_format = [](ReadonlyBytes nal_unit, u8 profile_idc, u8 chroma_format_idc, u8 bit_depth_luma) {
        auto parameter_set = Media::Codecs::H264::parse_sequence_parameter_set(nal_unit);
        EXPECT(parameter_set.has_value());
        if (!parameter_set.has_value())
            return;
        EXPECT_EQ(parameter_set->profile_idc, profile_idc);
        EXPECT_EQ(parameter_set->chroma_format_idc, chroma_format_idc);
        EXPECT_EQ(parameter_set->bit_depth_luma, bit_depth_luma);
    };

    // Baseline profile states no chroma format or bit depth, so they are inferred as 4:2:0 and 8-bit.
    Array<u8, 10> baseline { 0x67, 0x42, 0x00, 0x1e, 0x04, 0x17, 0x03, 0x60, 0xf6, 0x40 };
    expect_format(baseline, 66, 1, 8);

    // x264's sequence parameter sets for High, High 10, High 4:2:2 and High 4:4:4 Predictive streams.
    Array<u8, 27> high {
        0x67, 0x64, 0x00, 0x0d, 0xac, 0xd9, 0x41, 0x41, 0xfb, 0x01, 0x6a, 0x04, 0x04, 0x0a, 0x80, 0x00,
        0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x19, 0x07, 0x8a, 0x14, 0xcb
    };
    expect_format(high, 100, 1, 8);
    Array<u8, 29> high_10 {
        0x67, 0x6e, 0x00, 0x0d, 0xa6, 0xcd, 0x94, 0x14, 0x1f, 0xb0, 0x16, 0xa0, 0x40, 0x40, 0xa8, 0x00,
        0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x01, 0x90, 0x78, 0xa1, 0x4c, 0xb0
    };
    expect_format(high_10, 110, 1, 10);
    Array<u8, 27> high_422 {
        0x67, 0x7a, 0x00, 0x0d, 0xbc, 0xd9, 0x41, 0x41, 0xfb, 0x01, 0x6a, 0x04, 0x04, 0x0a, 0x80, 0x00,
        0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x19, 0x07, 0x8a, 0x14, 0xcb
    };
    expect_format(high_422, 122, 2, 8);
    Array<u8, 29> high_444 {
        0x67, 0xf4, 0x00, 0x0d, 0x91, 0x9b, 0x28, 0x28, 0x3f, 0x60, 0x2d, 0x40, 0x80, 0x81, 0x50, 0x00,
        0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x20, 0xf1, 0x42, 0x99, 0x60
    };
    expect_format(high_444, 244, 3, 8);
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

TEST_CASE(aac_configuration_record_refuses_sample_rates_outside_the_supported_range)
{
    auto create = [](u32 sample_rate, Optional<u32> spectral_band_replication_sample_rate = {}) {
        return Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, sample_rate, 2, spectral_band_replication_sample_rate);
    };

    EXPECT(!create(Media::MIN_AUDIO_SAMPLE_RATE).is_error());
    EXPECT(!create(Media::MAX_AUDIO_SAMPLE_RATE / 2, Media::MAX_AUDIO_SAMPLE_RATE).is_error());

    EXPECT_EQ(create(Media::MIN_AUDIO_SAMPLE_RATE - 1).error().category(), Media::DecoderErrorCategory::NotImplemented);
    EXPECT_EQ(create(Media::MAX_AUDIO_SAMPLE_RATE + 1).error().category(), Media::DecoderErrorCategory::NotImplemented);
    EXPECT_EQ(create(Media::MAX_AUDIO_SAMPLE_RATE / 2, Media::MAX_AUDIO_SAMPLE_RATE + 2).error().category(), Media::DecoderErrorCategory::NotImplemented);
    EXPECT_EQ(create(Media::MIN_AUDIO_SAMPLE_RATE - 1, (Media::MIN_AUDIO_SAMPLE_RATE - 1) * 2).error().category(), Media::DecoderErrorCategory::NotImplemented);
}

// G(13250, 34500) B(7500, 3000) R(34000, 16000) WP(15635, 16450) L(10000000, 50), as x265 is given them.
static void expect_h26x_mastering_display_color_volume(Media::ColorMetadata const& metadata)
{
    EXPECT(metadata.mastering_display_color_volume.has_value());
    if (metadata.mastering_display_color_volume.has_value()) {
        auto const& volume = metadata.mastering_display_color_volume.value();
        EXPECT_APPROXIMATE(volume.green.x, 0.265);
        EXPECT_APPROXIMATE(volume.green.y, 0.69);
        EXPECT_APPROXIMATE(volume.blue.x, 0.15);
        EXPECT_APPROXIMATE(volume.blue.y, 0.06);
        EXPECT_APPROXIMATE(volume.red.x, 0.68);
        EXPECT_APPROXIMATE(volume.red.y, 0.32);
        EXPECT_APPROXIMATE(volume.white_point.x, 0.3127);
        EXPECT_APPROXIMATE(volume.white_point.y, 0.329);
        EXPECT_APPROXIMATE(volume.maximum_luminance, 1000.0);
        EXPECT_APPROXIMATE(volume.minimum_luminance, 0.005);
    }
}

TEST_CASE(h265_prefix_sei_gives_the_mastering_display_and_content_light_levels)
{
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x4e, 0x01, // prefix SEI, layer 0
        0x05, 0x02, 0xaa, 0xbb, // an unregistered user data message, which is skipped
        0x89, 0x18, // mastering display colour volume
        0x33, 0xc2, 0x86, 0xc4, 0x1d, 0x4c, 0x0b, 0xb8, 0x84, 0xd0, 0x3e, 0x80,
        0x3d, 0x13, 0x40, 0x42,
        0x00, 0x98, 0x96, 0x80,
        0x00, 0x00, 0x03, 0x00, 0x32, // an emulation prevention byte follows the two zero bytes
        0x90, 0x04, // content light level information
        0x03, 0xe8, 0x01, 0x90,
        0x80, // RBSP trailing bits
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    expect_h26x_mastering_display_color_volume(metadata);
    EXPECT(metadata.content_light_level_information.has_value());
    if (metadata.content_light_level_information.has_value()) {
        EXPECT_EQ(metadata.content_light_level_information->maximum_content_light_level, 1000);
        EXPECT_EQ(metadata.content_light_level_information->maximum_frame_average_light_level, 400);
    }
}

TEST_CASE(h265_sei_message_running_past_its_nal_unit_is_ignored)
{
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x4e, 0x01,
        0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, // a complete content light level message
        0x89, 0x18, 0x33, 0xc2, // a mastering display colour volume with only one of its fields
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(!metadata.mastering_display_color_volume.has_value());
    EXPECT(metadata.content_light_level_information.has_value());
    if (metadata.content_light_level_information.has_value())
        EXPECT_EQ(metadata.content_light_level_information->maximum_content_light_level, 1000);
}

TEST_CASE(h265_sei_mastering_display_with_an_unspecified_peak_is_left_out)
{
    // A maximum luminance code of 200,000,000 is outside the range H.265 gives meaning to.
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x4e, 0x01,
        0x89, 0x18,
        0x33, 0xc2, 0x86, 0xc4, 0x1d, 0x4c, 0x0b, 0xb8, 0x84, 0xd0, 0x3e, 0x80, 0x3d, 0x13, 0x40, 0x42,
        0x0b, 0xeb, 0xc2, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x32,
        0x90, 0x04, 0x03, 0xe8, 0x01, 0x90,
        0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(!metadata.mastering_display_color_volume.has_value());
    EXPECT(metadata.content_light_level_information.has_value());
}

TEST_CASE(h265_sei_mastering_display_with_a_zero_minimum_is_kept)
{
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x4e, 0x01,
        0x89, 0x18,
        0x33, 0xc2, 0x86, 0xc4, 0x1d, 0x4c, 0x0b, 0xb8, 0x84, 0xd0, 0x3e, 0x80, 0x3d, 0x13, 0x40, 0x42,
        0x00, 0x98, 0x96, 0x80,
        0x00, 0x00, 0x03, 0x00, 0x00,
        0x90, 0x04, 0x03, 0xe8, 0x01, 0x90,
        0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(metadata.mastering_display_color_volume.has_value());
    if (metadata.mastering_display_color_volume.has_value()) {
        EXPECT_APPROXIMATE(metadata.mastering_display_color_volume->maximum_luminance, 1000.0);
        EXPECT_EQ(metadata.mastering_display_color_volume->minimum_luminance, 0.0f);
    }
}

TEST_CASE(h265_sei_message_whose_size_overflows_is_ignored)
{
    // Without its overflow being caught, a size of 16,843,009 0xFF bytes and a 0x05 adds up to 4.
    ByteBuffer nal_unit;
    nal_unit.append(0x4e);
    nal_unit.append(0x01);
    nal_unit.append(0x90);
    MUST(nal_unit.try_resize(nal_unit.size() + 16'843'009));
    nal_unit.bytes().slice(3).fill(0xff);
    for (u8 byte : { 0x05, 0x03, 0xe8, 0x01, 0x90, 0x80 })
        nal_unit.append(byte);

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(!metadata.content_light_level_information.has_value());
}

TEST_CASE(h264_nal_unit_with_the_forbidden_zero_bit_set_gives_nothing)
{
    auto nal_unit = to_array<u8>({ 0x86, 0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, 0x80 });
    EXPECT(!Media::Codecs::H264::high_dynamic_range_metadata_of_nal_unit(nal_unit).content_light_level_information.has_value());
}

TEST_CASE(h265_sei_message_running_into_the_trailing_bits_is_ignored)
{
    // A content light level message missing its last byte, which the RBSP stop byte would otherwise complete.
    auto nal_unit = to_array<u8>({ 0x4e, 0x01, 0x90, 0x04, 0x03, 0xe8, 0x01, 0x80 });
    EXPECT(!Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit).content_light_level_information.has_value());
}

TEST_CASE(h265_nal_units_other_than_base_layer_prefix_sei_give_nothing)
{
    // A suffix SEI, a prefix SEI in layer 1 and a sequence parameter set, each followed by a content light level.
    // clang-format off
    Array<Array<u8, 9>, 3> nal_units { {
        { 0x50, 0x01, 0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, 0x80 },
        { 0x4e, 0x09, 0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, 0x80 },
        { 0x42, 0x01, 0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, 0x80 },
    } };
    // clang-format on

    for (auto const& nal_unit : nal_units) {
        auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
        EXPECT(!metadata.content_light_level_information.has_value());
    }
}

TEST_CASE(h264_sei_gives_the_mastering_display_and_content_light_levels)
{
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x06, // SEI
        0x90, 0x04, 0x03, 0xe8, 0x01, 0x90,
        0x89, 0x18,
        0x33, 0xc2, 0x86, 0xc4, 0x1d, 0x4c, 0x0b, 0xb8, 0x84, 0xd0, 0x3e, 0x80,
        0x3d, 0x13, 0x40, 0x42,
        0x00, 0x98, 0x96, 0x80,
        0x00, 0x00, 0x03, 0x00, 0x32,
        0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H264::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    expect_h26x_mastering_display_color_volume(metadata);
    EXPECT(metadata.content_light_level_information.has_value());
    if (metadata.content_light_level_information.has_value())
        EXPECT_EQ(metadata.content_light_level_information->maximum_frame_average_light_level, 400);
}

TEST_CASE(av1_metadata_obus_give_the_mastering_display_and_content_light_levels)
{
    // clang-format off
    auto obus = to_array<u8>({
        0x12, 0x00, // a temporal delimiter, which is skipped
        0x2a, 0x06, // metadata
        0x01, 0x03, 0xed, 0x01, 0x3b, // HDR_CLL: 1005 and 315
        0x80, // trailing bits
        0x2a, 0x1a, // metadata
        0x02, // HDR_MDCV
        0xb5, 0x40, 0x4a, 0xc0, // red (46400, 19136)
        0x2b, 0x85, 0xcc, 0x08, // green (11141, 52232)
        0x21, 0x8a, 0x0b, 0xc7, // blue (8586, 3015)
        0x50, 0x0d, 0x54, 0x39, // white point (20493, 21561)
        0x00, 0x03, 0xe8, 0x00, // 1000 cd/m² in 24.8
        0x00, 0x00, 0x00, 0x52, // 82 / 2^14 cd/m² in 18.14
        0x80, // trailing bits
    });
    // clang-format on

    auto metadata = Media::Codecs::AV1::high_dynamic_range_metadata_of_obus(obus);
    EXPECT(metadata.content_light_level_information.has_value());
    if (metadata.content_light_level_information.has_value()) {
        EXPECT_EQ(metadata.content_light_level_information->maximum_content_light_level, 1005);
        EXPECT_EQ(metadata.content_light_level_information->maximum_frame_average_light_level, 315);
    }

    EXPECT(metadata.mastering_display_color_volume.has_value());
    if (metadata.mastering_display_color_volume.has_value()) {
        auto const& volume = metadata.mastering_display_color_volume.value();
        EXPECT_APPROXIMATE(volume.red.x, 46400 / 65536.0);
        EXPECT_APPROXIMATE(volume.red.y, 19136 / 65536.0);
        EXPECT_APPROXIMATE(volume.green.x, 11141 / 65536.0);
        EXPECT_APPROXIMATE(volume.green.y, 52232 / 65536.0);
        EXPECT_APPROXIMATE(volume.blue.x, 8586 / 65536.0);
        EXPECT_APPROXIMATE(volume.blue.y, 3015 / 65536.0);
        EXPECT_APPROXIMATE(volume.white_point.x, 20493 / 65536.0);
        EXPECT_APPROXIMATE(volume.white_point.y, 21561 / 65536.0);
        EXPECT_APPROXIMATE(volume.maximum_luminance, 1000.0);
        EXPECT_APPROXIMATE(volume.minimum_luminance, 82 / 16384.0);
    }
}

TEST_CASE(av1_metadata_obu_running_into_its_trailing_bits_is_ignored)
{
    // An HDR_CLL missing its last byte, which the trailing bits would otherwise complete.
    auto obus = to_array<u8>({ 0x2a, 0x05, 0x01, 0x03, 0xed, 0x01, 0x80 });
    EXPECT(!Media::Codecs::AV1::high_dynamic_range_metadata_of_obus(obus).content_light_level_information.has_value());
}

TEST_CASE(av1_obus_after_a_malformed_one_are_not_read)
{
    // clang-format off
    auto obus = to_array<u8>({
        0x2a, 0x10, 0x01, // metadata claiming more payload than there is
        0x2a, 0x06, 0x01, 0x03, 0xed, 0x01, 0x3b, 0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::AV1::high_dynamic_range_metadata_of_obus(obus);
    EXPECT(!metadata.content_light_level_information.has_value());
}

// One processing window whose maxscl is (1294, 3641, 1064), in units of 0.00001 of 10000 cd/m².
// clang-format off
static constexpr auto hdr10_plus_with_one_window = to_array<u8>({
    0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0c, 0x80, 0x0a, 0x1c, 0x0e, 0x39, 0x02,
    0x14, 0x01, 0x0a, 0x04, 0x08, 0x01, 0x90, 0x00, 0x00,
});
// clang-format on

TEST_CASE(hdr10_plus_gives_the_brightest_component_of_the_first_window)
{
    auto luminance = Media::Codecs::hdr10_plus_maximum_scene_luminance(hdr10_plus_with_one_window);
    EXPECT(luminance.has_value());
    if (luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(luminance.value(), 364.1, 0.01);
}

TEST_CASE(hdr10_plus_skips_further_windows_and_the_actual_peak_luminances)
{
    // Three windows, the later two with a maxscl of 99999, and a three by two actual peak luminance matrix.
    // clang-format off
    auto message = to_array<u8>({
        0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0xc4, 0x8d, 0x04, 0x8d, 0x04, 0x8d, 0x04, 0x8d, 0x04,
        0x8d, 0x04, 0x8d, 0x15, 0x9e, 0x26, 0x9e, 0x26, 0x9e, 0x26, 0xa2, 0x46, 0x82, 0x46, 0x82, 0x46,
        0x82, 0x46, 0x82, 0x46, 0x82, 0x46, 0x8a, 0xcf, 0x13, 0x4f, 0x13, 0x4f, 0x13, 0x50, 0x00, 0x03,
        0x21, 0x18, 0x80, 0x48, 0xd1, 0x40, 0xa1, 0xc0, 0xe3, 0x90, 0x21, 0x40, 0x10, 0xa0, 0x40, 0x80,
        0x19, 0x00, 0x0c, 0x34, 0xfe, 0x1a, 0x7f, 0x0d, 0x3f, 0x86, 0x9f, 0x10, 0x20, 0x06, 0x40, 0x03,
        0x0d, 0x3f, 0x86, 0x9f, 0xc3, 0x4f, 0xe1, 0xa7, 0xc4, 0x08, 0x01, 0x90, 0x00, 0x00,
    });
    // clang-format on

    auto luminance = Media::Codecs::hdr10_plus_maximum_scene_luminance(message);
    EXPECT(luminance.has_value());
    if (luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(luminance.value(), 364.1, 0.01);
}

TEST_CASE(hdr10_plus_rejects_other_messages_and_unknown_versions)
{
    auto other_provider = hdr10_plus_with_one_window;
    other_provider[2] = 0x90;
    EXPECT(!Media::Codecs::hdr10_plus_maximum_scene_luminance(other_provider).has_value());

    auto later_version = hdr10_plus_with_one_window;
    later_version[6] = 0x02;
    EXPECT(!Media::Codecs::hdr10_plus_maximum_scene_luminance(later_version).has_value());

    auto truncated = ReadonlyBytes { hdr10_plus_with_one_window }.trim(12);
    EXPECT(!Media::Codecs::hdr10_plus_maximum_scene_luminance(truncated).has_value());
}

TEST_CASE(hdr10_plus_maxscl_beyond_its_range_gives_nothing)
{
    // A maxscl of 120,000 fits the 17-bit field, but HDR10+ only gives meaning to values up to 100,000.
    // clang-format off
    auto message = to_array<u8>({
        0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0c, 0x83, 0xa9, 0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x04, 0x08, 0x01, 0x90, 0x00, 0x00,
    });
    // clang-format on

    EXPECT(!Media::Codecs::hdr10_plus_maximum_scene_luminance(message).has_value());
}

TEST_CASE(hdr10_plus_with_an_unknown_peak_gives_nothing)
{
    // clang-format off
    auto message = to_array<u8>({
        0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0c, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x04, 0x08, 0x01, 0x90, 0x00, 0x00,
    });
    // clang-format on

    EXPECT(!Media::Codecs::hdr10_plus_maximum_scene_luminance(message).has_value());
}

TEST_CASE(h265_prefix_sei_gives_the_hdr10_plus_scene_peak)
{
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x4e, 0x01, // prefix SEI, layer 0
        0x04, 0x19, // user data registered by ITU-T T.35
        0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0c, 0x80, 0x0a, 0x1c, 0x0e, 0x39, 0x02,
        0x14, 0x01, 0x0a, 0x04, 0x08, 0x01, 0x90, 0x00, 0x00,
        0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, // content light level information
        0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(metadata.maximum_scene_luminance.has_value());
    if (metadata.maximum_scene_luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(metadata.maximum_scene_luminance.value(), 364.1, 0.01);
    EXPECT(metadata.content_light_level_information.has_value());
}

TEST_CASE(h265_sei_itu_t_t35_message_too_large_to_be_hdr10_plus_is_skipped)
{
    // HDR10+ that would still read as such with the padding after it, then a content light level.
    ByteBuffer message;
    message.append(hdr10_plus_with_one_window.span());
    while (message.size() <= Media::Codecs::HDR10_PLUS_MAXIMUM_T35_MESSAGE_SIZE)
        message.append(0x11);

    ByteBuffer nal_unit;
    for (u8 byte : { 0x4e, 0x01, 0x04 })
        nal_unit.append(byte);
    for (auto size = message.size(); true; size -= 0xff) {
        nal_unit.append(static_cast<u8>(min<size_t>(size, 0xff)));
        if (size < 0xff)
            break;
    }
    nal_unit.append(message);
    for (u8 byte : { 0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, 0x80 })
        nal_unit.append(byte);

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(!metadata.maximum_scene_luminance.has_value());
    EXPECT(metadata.content_light_level_information.has_value());
}

TEST_CASE(av1_metadata_obu_gives_the_hdr10_plus_scene_peak)
{
    // clang-format off
    auto obus = to_array<u8>({
        0x2a, 0x1b, // metadata
        0x04, // ITUT_T35
        0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0c, 0x80, 0x0a, 0x1c, 0x0e, 0x39, 0x02,
        0x14, 0x01, 0x0a, 0x04, 0x08, 0x01, 0x90, 0x00, 0x00,
        0x80, // trailing bits
    });
    // clang-format on

    auto metadata = Media::Codecs::AV1::high_dynamic_range_metadata_of_obus(obus);
    EXPECT(metadata.maximum_scene_luminance.has_value());
    if (metadata.maximum_scene_luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(metadata.maximum_scene_luminance.value(), 364.1, 0.01);
}

TEST_CASE(h265_prefix_sei_hdr10_plus_drops_its_emulation_prevention_bytes)
{
    // The maxscl (0, 0, 3641) codes two zero bytes followed by a byte below 4, so an emulation prevention byte splits it.
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x4e, 0x01,
        0x04, 0x19,
        0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04, 0x01, 0x40, 0x00, 0x0c, 0x80, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x07, 0x1c, 0x80, 0x00, 0x04, 0x08, 0x01, 0x90, 0x00, 0x00,
        0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(metadata.maximum_scene_luminance.has_value());
    if (metadata.maximum_scene_luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(metadata.maximum_scene_luminance.value(), 364.1, 0.01);

    // Read with the emulation prevention byte left in, the same message gives another peak.
    auto escaped_message = Media::Codecs::hdr10_plus_maximum_scene_luminance(ReadonlyBytes { nal_unit }.slice(4, 26));
    EXPECT(!escaped_message.has_value() || fabsf(escaped_message.value() - 364.1f) > 0.01f);
}

// These are the first two RPUs of Chromium's glass-blowing2-dolby-vision-profile-8-1-frag.mp4, whose level 1 maximums
// FFmpeg reads as 3615 and 3687.
TEST_CASE(h265_dolby_vision_nal_unit_gives_the_level_1_scene_peak)
{
    // clang-format off
    auto first_nal_unit = to_array<u8>({
        0x7c, 0x01, 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0x50, 0x6f, 0x00, 0x3f, 0xf8, 0x01, 0xff,
        0xc0, 0x0f, 0xff, 0xd0, 0x00, 0x00, 0x08, 0x00, 0x00, 0x06, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00,
        0x34, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x03, 0x01, 0xc9, 0x59, 0x80, 0x00, 0x0d, 0x7a, 0x89,
        0x59, 0xbe, 0x7f, 0x3a, 0xc7, 0x09, 0x59, 0x91, 0x32, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x03,
        0x02, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x07, 0x0d, 0x88, 0x90, 0xc0, 0x61,
        0x82, 0x97, 0x8c, 0x23, 0x81, 0x45, 0x00, 0x00, 0x03, 0x00, 0x69, 0x8f, 0x96, 0xbf, 0xff, 0xc0,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x18, 0x08, 0x1f, 0x73, 0x80, 0x54,
        0x60, 0x30, 0x08, 0x01, 0x70, 0xfa, 0xf6, 0x00, 0xc0, 0x28, 0x21, 0x80, 0x08, 0x00, 0x80, 0x06,
        0x06, 0x80, 0x01, 0x00, 0x02, 0x02, 0x28, 0x39, 0x1e, 0x88, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x03, 0x00, 0x12, 0x0c, 0x1f, 0x40, 0x00, 0x64, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x59, 0xfc, 0x25, 0x44, 0x80,
    });
    auto second_nal_unit = to_array<u8>({
        0x7c, 0x01, 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0x50, 0x6f, 0x00, 0x3f, 0xf8, 0x01, 0xff,
        0xc0, 0x0f, 0xff, 0xd0, 0x00, 0x00, 0x08, 0x00, 0x00, 0x06, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00,
        0x34, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x03, 0x01, 0xc9, 0x59, 0x80, 0x00, 0x0d, 0x7a, 0x89,
        0x59, 0xbe, 0x7f, 0x3a, 0xc7, 0x09, 0x59, 0x91, 0x32, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x03,
        0x02, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x07, 0x0d, 0x88, 0x90, 0xc0, 0x61,
        0x82, 0x97, 0x8c, 0x23, 0x81, 0x45, 0x00, 0x00, 0x03, 0x00, 0x69, 0x8f, 0x96, 0xbf, 0xff, 0xc0,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x18, 0x08, 0x1f, 0x73, 0x80, 0x54,
        0x60, 0x30, 0x08, 0x01, 0x73, 0x3b, 0x18, 0x00, 0xc0, 0x28, 0x21, 0x80, 0x08, 0x00, 0x80, 0x06,
        0xa2, 0x6c, 0xb1, 0x00, 0x02, 0x02, 0x30, 0xb9, 0xd9, 0x88, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x03, 0x00, 0x12, 0x0c, 0x1f, 0x40, 0x00, 0x64, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x4b, 0xf6, 0x1b, 0xb2, 0x80,
    });
    // clang-format on

    auto first = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(first_nal_unit);
    EXPECT(first.maximum_scene_luminance.has_value());
    if (first.maximum_scene_luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(first.maximum_scene_luminance.value(), 3330.0, 0.5);
    auto second = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(second_nal_unit);
    EXPECT(second.maximum_scene_luminance.has_value());
    if (second.maximum_scene_luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(second.maximum_scene_luminance.value(), 3918.9, 0.5);
}

TEST_CASE(h265_dolby_vision_nal_unit_reusing_a_mapping_gives_the_level_1_scene_peak)
{
    // An RPU that reuses an earlier one's mapping and leaves out the colour transform, with a level 1 maximum of 2851.
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x7c, 0x01, 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0xc5, 0xfa, 0x60, 0x18, 0x05, 0x04, 0x30,
        0x43, 0x04, 0x30, 0x43, 0x04, 0x30, 0x42, 0xff, 0xf0, 0x60, 0x10, 0x0c, 0xb2, 0x33, 0x33, 0x00,
        0x83, 0xd7, 0xab, 0x17, 0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(metadata.maximum_scene_luminance.has_value());
    if (metadata.maximum_scene_luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(metadata.maximum_scene_luminance.value(), 599.56, 0.5);
}

TEST_CASE(h265_dolby_vision_nal_unit_with_a_zero_level_1_maximum_gives_nothing)
{
    // The RPU above, with its level 1 maximum cleared.
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x7c, 0x01, 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0xc5, 0xfa, 0x60, 0x18, 0x05, 0x04, 0x30,
        0x43, 0x04, 0x30, 0x43, 0x04, 0x30, 0x42, 0xff, 0xf0, 0x60, 0x10, 0x0c, 0x00, 0x03, 0x33, 0x00,
        0x83, 0xd7, 0xab, 0x17, 0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(!metadata.maximum_scene_luminance.has_value());
}

TEST_CASE(h265_dolby_vision_level_1_block_shorter_than_its_fields_gives_nothing)
{
    // The RPU's level 1 block declares two bytes, which cannot hold its three 12-bit fields.
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x7c, 0x01, 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0xc5, 0xfa, 0x60, 0x18, 0x05, 0x04, 0x30,
        0x43, 0x04, 0x30, 0x43, 0x04, 0x30, 0x42, 0xff, 0xf0, 0xc0, 0x40, 0x32, 0xb4, 0xcc, 0xcc, 0x00,
        0x4a, 0x2b, 0x73, 0x8c, 0x80,
    });
    // clang-format on

    EXPECT(!Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit).maximum_scene_luminance.has_value());
}

TEST_CASE(h265_dolby_vision_nal_unit_without_display_management_gives_nothing)
{
    // clang-format off
    auto nal_unit = to_array<u8>({
        0x7c, 0x01, 0x19, 0x08, 0x09, 0x08, 0x40, 0x61, 0x36, 0xc1, 0x3c, 0x00, 0xff, 0xe0, 0x07, 0xff,
        0x00, 0x3f, 0xff, 0x40, 0x00, 0x00, 0x20, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00,
        0x03, 0x00, 0xd0, 0x00, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0x0f, 0x3e, 0x9d, 0x3e, 0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
    EXPECT(!metadata.maximum_scene_luminance.has_value());
}

TEST_CASE(av1_metadata_obu_gives_the_dolby_vision_scene_peak)
{
    // An ITU-T T.35 metadata OBU holding an EMDF-wrapped RPU with a level 1 maximum of 2081.
    // clang-format off
    auto obus = to_array<u8>({
        0x2a, 0x98, 0x01, 0x04, 0xb5, 0x00, 0x3b, 0x00, 0x00, 0x08, 0x00, 0x37, 0xcd, 0x08, 0x31, 0x00,
        0x80, 0x90, 0x84, 0x06, 0x13, 0x6c, 0x1b, 0xc0, 0x0f, 0xfe, 0x00, 0x7f, 0xf0, 0x03, 0xff, 0xf4,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0xa0, 0x00, 0x00, 0x10, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x68, 0x95, 0x98, 0x00, 0x00, 0xd7, 0xa8, 0x95, 0x9b, 0xe7, 0xf3, 0xac, 0x70,
        0x95, 0x99, 0x13, 0x28, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00,
        0x00, 0x00, 0x70, 0xd8, 0x89, 0x0c, 0x06, 0x18, 0x29, 0x78, 0xc2, 0x38, 0x14, 0x50, 0x00, 0x00,
        0x06, 0x98, 0xf9, 0x6b, 0xff, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01,
        0xf7, 0x38, 0x05, 0x44, 0x01, 0x80, 0x50, 0x43, 0x04, 0x30, 0x43, 0x04, 0x30, 0x43, 0x04, 0x2f,
        0xff, 0x06, 0x01, 0x00, 0xc8, 0x21, 0x33, 0x30, 0x12, 0x0c, 0x07, 0xd0, 0x00, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x03, 0x14, 0xde, 0x09, 0xe8, 0x00, 0x20, 0x00, 0x80,
    });
    // clang-format on

    auto metadata = Media::Codecs::AV1::high_dynamic_range_metadata_of_obus(obus);
    EXPECT(metadata.maximum_scene_luminance.has_value());
    if (metadata.maximum_scene_luminance.has_value())
        EXPECT_APPROXIMATE_WITH_ERROR(metadata.maximum_scene_luminance.value(), 100.1, 0.5);
}

TEST_CASE(h265_dolby_vision_rpus_of_every_kind_give_their_scene_peaks)
{
    ScenePeakChecker scene_peaks { hevc_dolby_vision_rpus_scene_peaks };
    auto file = MUST(Core::File::open("./hevc_dolby_vision_rpus.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).release_value();
    MUST(demuxer->create_context_for_track(track));

    u8 nal_unit_length_size = 4;
    size_t sample_count = 0;
    while (true) {
        auto sample_result = demuxer->get_next_sample_for_track(track);
        if (sample_result.is_error()) {
            EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        auto const& sample = sample_result.value();
        if (auto configuration = sample.new_codec_configuration(); configuration.has_value())
            nal_unit_length_size = Media::Codecs::H265::parse_parameter_sets_from_configuration_record(*configuration).value().nal_unit_length_size;

        Media::ColorMetadata metadata;
        Media::Codecs::NALUnitIterator iterator { sample.data(), nal_unit_length_size };
        for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next())
            metadata.adopt_specified_values(Media::Codecs::H265::high_dynamic_range_metadata_of_nal_unit(*nal_unit));
        scene_peaks.check(sample.presentation_timestamp(), metadata.maximum_scene_luminance);
        sample_count++;
    }
    EXPECT_EQ(sample_count, 6u);
    scene_peaks.expect_every_frame_checked();
}

// Length-prefixed H.265 NAL units: content light level SEIs, slices of IDR, trailing and CRA pictures, and an end of
// sequence.
// clang-format off
constexpr auto h265_content_light_level_sei = to_array<u8>({ 0x00, 0x00, 0x00, 0x09, 0x4e, 0x01, 0x90, 0x04, 0x03, 0xe8, 0x01, 0x90, 0x80 });
constexpr auto h265_unknown_peak_content_light_level_sei = to_array<u8>({ 0x00, 0x00, 0x00, 0x0a, 0x4e, 0x01, 0x90, 0x04, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x80 });
constexpr auto h265_idr_slice = to_array<u8>({ 0x00, 0x00, 0x00, 0x03, 0x26, 0x01, 0xaf });
constexpr auto h265_trailing_slice = to_array<u8>({ 0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0xaf });
constexpr auto h265_clean_random_access_slice = to_array<u8>({ 0x00, 0x00, 0x00, 0x03, 0x2a, 0x01, 0xaf });
constexpr auto h265_end_of_sequence = to_array<u8>({ 0x00, 0x00, 0x00, 0x02, 0x48, 0x01 });
// clang-format on

static Media::CodedFrame h265_coded_frame(Vector<ReadonlyBytes> nal_units, i64 milliseconds)
{
    ByteBuffer data;
    for (auto nal_unit : nal_units)
        data.append(nal_unit);
    auto timestamp = AK::Duration::from_milliseconds(milliseconds);
    return Media::CodedFrame(Media::CodecID::H265, timestamp, timestamp, AK::Duration::from_milliseconds(33), Media::FrameFlags::Keyframe, MUST(FixedArray<u8>::create(data.bytes())));
}

TEST_CASE(h265_static_metadata_ends_with_its_coded_video_sequence)
{
    Media::Codecs::HighDynamicRangeMetadataTracker tracker { Media::CodecID::H265, {} };
    EXPECT(tracker.apply_coded_frame(h265_coded_frame({ h265_content_light_level_sei, h265_idr_slice }, 0)).content_light_level_information.has_value());
    EXPECT(tracker.apply_coded_frame(h265_coded_frame({ h265_trailing_slice }, 33)).content_light_level_information.has_value());
    EXPECT(!tracker.apply_coded_frame(h265_coded_frame({ h265_idr_slice }, 67)).content_light_level_information.has_value());
}

TEST_CASE(h265_static_metadata_ends_at_an_end_of_sequence)
{
    Media::Codecs::HighDynamicRangeMetadataTracker tracker { Media::CodecID::H265, {} };
    EXPECT(tracker.apply_coded_frame(h265_coded_frame({ h265_content_light_level_sei, h265_idr_slice }, 0)).content_light_level_information.has_value());
    // A CRA picture alone may be in the middle of a coded video sequence.
    EXPECT(tracker.apply_coded_frame(h265_coded_frame({ h265_clean_random_access_slice }, 33)).content_light_level_information.has_value());
    EXPECT(tracker.apply_coded_frame(h265_coded_frame({ h265_trailing_slice, h265_end_of_sequence }, 67)).content_light_level_information.has_value());
    EXPECT(!tracker.apply_coded_frame(h265_coded_frame({ h265_clean_random_access_slice }, 100)).content_light_level_information.has_value());
}

TEST_CASE(h265_content_light_level_message_replaces_the_previous_one)
{
    Media::Codecs::HighDynamicRangeMetadataTracker tracker { Media::CodecID::H265, {} };
    tracker.apply_coded_frame(h265_coded_frame({ h265_content_light_level_sei, h265_idr_slice }, 0));
    auto const& metadata = tracker.apply_coded_frame(h265_coded_frame({ h265_unknown_peak_content_light_level_sei, h265_idr_slice }, 33));
    EXPECT(metadata.content_light_level_information.has_value());
    if (metadata.content_light_level_information.has_value()) {
        EXPECT_EQ(metadata.content_light_level_information->maximum_content_light_level, 0);
        EXPECT_EQ(metadata.content_light_level_information->maximum_frame_average_light_level, 200);
    }
}
