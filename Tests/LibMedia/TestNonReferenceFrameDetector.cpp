/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FixedArray.h>
#include <AK/Vector.h>
#include <LibMediaCodecs/Codecs/H264.h>
#include <LibMediaCodecs/Codecs/H265.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/NonReferenceFrameDetector.h>
#include <LibTest/TestCase.h>

using H264 = Media::Codecs::H264;
using H265 = Media::Codecs::H265;

static FixedArray<u8> length_prefixed_nal_units(Vector<ReadonlyBytes> const& nal_units)
{
    Vector<u8> data;
    for (auto nal_unit : nal_units) {
        auto size = static_cast<u32>(nal_unit.size());
        data.append(static_cast<u8>(size >> 24));
        data.append(static_cast<u8>(size >> 16));
        data.append(static_cast<u8>(size >> 8));
        data.append(static_cast<u8>(size));
        data.append(nal_unit.data(), nal_unit.size());
    }
    return MUST(FixedArray<u8>::create(data.span()));
}

static Media::CodedFrame coded_frame(Media::CodecID codec_id, Vector<ReadonlyBytes> const& nal_units, Optional<ReadonlyBytes> configuration = {})
{
    Optional<FixedArray<u8>> configuration_storage;
    if (configuration.has_value())
        configuration_storage = MUST(FixedArray<u8>::create(*configuration));
    return Media::CodedFrame(codec_id, {}, {}, {}, Media::FrameFlags::None, length_prefixed_nal_units(nal_units), move(configuration_storage));
}

// The slice data after each header is irrelevant, since only the headers are read.
static constexpr Array<u8, 2> H264_REFERENCE_SLICE { 0x41, 0x9a };
static constexpr Array<u8, 2> H264_NON_REFERENCE_SLICE { 0x01, 0x9e };
static constexpr Array<u8, 2> H264_IDR_SLICE { 0x65, 0x88 };
static constexpr Array<u8, 2> H264_SUPPLEMENTAL_ENHANCEMENT_INFORMATION { 0x06, 0x05 };

static ReadonlyBytes h264_configuration() { return H264::representative_configuration_record_for_profile(H264::Profile::High); }

TEST_CASE(h264_reports_frames_whose_slices_are_all_non_reference)
{
    Media::NonReferenceFrameDetector detector;
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_IDR_SLICE }, h264_configuration())));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_REFERENCE_SLICE })));
    EXPECT(detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_NON_REFERENCE_SLICE })));
    EXPECT(detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_SUPPLEMENTAL_ENHANCEMENT_INFORMATION, H264_NON_REFERENCE_SLICE, H264_NON_REFERENCE_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_NON_REFERENCE_SLICE, H264_REFERENCE_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_SUPPLEMENTAL_ENHANCEMENT_INFORMATION })));
}

TEST_CASE(h264_passes_non_reference_frames_carrying_parameter_sets)
{
    Media::NonReferenceFrameDetector detector;
    auto sets = H264::parse_parameter_sets_from_configuration_record(h264_configuration());
    VERIFY(sets.has_value());
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_IDR_SLICE }, h264_configuration())));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { sets->sequence[0], H264_NON_REFERENCE_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { sets->picture[0], H264_NON_REFERENCE_SLICE })));
    EXPECT(detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_NON_REFERENCE_SLICE })));
}

TEST_CASE(h264_reports_nothing_without_a_usable_configuration)
{
    Media::NonReferenceFrameDetector detector;
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_NON_REFERENCE_SLICE })));

    Array<u8, 1> truncated_configuration { 0x01 };
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_NON_REFERENCE_SLICE }, truncated_configuration.span())));
}

TEST_CASE(h264_reports_nothing_for_frames_that_cannot_be_split)
{
    Media::NonReferenceFrameDetector detector;
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_IDR_SLICE }, h264_configuration())));

    auto frame_data = length_prefixed_nal_units({ H264_NON_REFERENCE_SLICE });
    auto overrunning_data = MUST(FixedArray<u8>::create(frame_data.span()));
    overrunning_data[3] = 0xff;
    EXPECT(!detector.frame_is_non_reference(Media::CodedFrame(Media::CodecID::H264, {}, {}, {}, Media::FrameFlags::None, move(overrunning_data))));
}

TEST_CASE(a_codec_change_without_a_configuration_stops_reports)
{
    Media::NonReferenceFrameDetector detector;
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_IDR_SLICE }, h264_configuration())));
    EXPECT(detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_NON_REFERENCE_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::VP9, { H264_NON_REFERENCE_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H264, { H264_NON_REFERENCE_SLICE })));
}

// Two-byte headers: the type in bits 1 to 6 of the first byte, and TemporalId + 1 in the low bits of the second.
static constexpr Array<u8, 3> H265_IDR_SLICE { 0x26, 0x01, 0xaf };
static constexpr Array<u8, 3> H265_TRAIL_R_SLICE { 0x02, 0x01, 0xd0 };
static constexpr Array<u8, 3> H265_TRAIL_N_SLICE { 0x00, 0x01, 0xd0 };
static constexpr Array<u8, 3> H265_TRAIL_N_SLICE_AT_TEMPORAL_ID_1 { 0x00, 0x02, 0xd0 };
static constexpr Array<u8, 3> H265_RASL_N_SLICE { 0x10, 0x01, 0xd0 };

static ReadonlyBytes h265_configuration() { return H265::representative_configuration_record_for_profile(H265::Profile::Main); }

TEST_CASE(h265_reports_sub_layer_non_reference_frames_at_the_highest_temporal_id)
{
    Media::NonReferenceFrameDetector detector;
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_IDR_SLICE }, h265_configuration())));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_TRAIL_R_SLICE })));
    EXPECT(detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_TRAIL_N_SLICE })));
    EXPECT(detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_RASL_N_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_TRAIL_N_SLICE, H265_TRAIL_R_SLICE })));
    // The configuration's SPS allows a single sub-layer, so a picture above it says nothing about what references it.
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_TRAIL_N_SLICE_AT_TEMPORAL_ID_1 })));
}

TEST_CASE(h265_passes_non_reference_frames_carrying_parameter_sets)
{
    Media::NonReferenceFrameDetector detector;
    auto sets = H265::parse_parameter_sets_from_configuration_record(h265_configuration());
    VERIFY(sets.has_value());
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_IDR_SLICE }, h265_configuration())));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { sets->video[0], H265_TRAIL_N_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { sets->sequence[0], H265_TRAIL_N_SLICE })));
    EXPECT(!detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { sets->picture[0], H265_TRAIL_N_SLICE })));
    EXPECT(detector.frame_is_non_reference(coded_frame(Media::CodecID::H265, { H265_TRAIL_N_SLICE })));
}
