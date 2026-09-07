/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibMedia/BitReader.h>
#include <LibMedia/Codecs/CodecString.h>
#include <LibMedia/Codecs/H264.h>
#include <LibMedia/Codecs/NALUnit.h>
#include <LibMedia/Codecs/RBSPBitReader.h>

namespace Media::Codecs {

Optional<H264::Parameters> H264::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};

    auto profile_idc = consume_two_digit_hexadecimal(lexer);
    auto constraint_set_flags = consume_two_digit_hexadecimal(lexer);
    auto level_idc = consume_two_digit_hexadecimal(lexer);
    if (!profile_idc.has_value() || !constraint_set_flags.has_value() || !level_idc.has_value() || !lexer.is_eof())
        return {};

    return Parameters {
        *profile_idc,
        *constraint_set_flags,
        *level_idc,
    };
}

Optional<H264::Parameters> H264::parse_configuration_record(ReadonlyBytes record)
{
    static constexpr size_t MINIMUM_RECORD_SIZE = 4;
    if (record.size() < MINIMUM_RECORD_SIZE)
        return {};

    // Version 1 is the only version defined, and is what distinguishes a configuration record from Annex B data.
    if (record[0] != 1)
        return {};

    return Parameters {
        record[1],
        record[2],
        record[3],
    };
}

static constexpr size_t NAL_UNIT_HEADER_SIZE = 1;
static constexpr u8 NAL_UNIT_TYPE_MASK = 0x1F;
static constexpr u8 SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE = 7;
static constexpr u8 PICTURE_PARAMETER_SET_NAL_UNIT_TYPE = 8;

// The profiles whose sequence parameter sets carry the chroma format and scaling matrix fields.
// ITU-T H.264 (08/2024), 7.3.2.1.1.
static bool profile_has_chroma_format_fields(u8 profile_idc)
{
    return first_is_one_of(profile_idc, 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135);
}

// ITU-T H.264 (08/2024), 7.3.2.1.1.1: scaling_list().
static void skip_scaling_list(Codecs::RBSPBitReader& reader, size_t entry_count)
{
    i32 last_scale = 8;
    i32 next_scale = 8;
    for (size_t index = 0; index < entry_count; index++) {
        if (next_scale != 0)
            next_scale = (last_scale + reader.read_signed_exp_golomb() + 256) % 256;
        if (next_scale != 0)
            last_scale = next_scale;
    }
}

// ITU-T H.264 (08/2024), E.1.2: hrd_parameters().
static void skip_hrd_parameters(Codecs::RBSPBitReader& reader)
{
    auto coded_picture_buffer_count = reader.read_unsigned_exp_golomb() + 1;
    reader.skip_bits(4); // bit_rate_scale
    reader.skip_bits(4); // cpb_size_scale
    for (u32 index = 0; index < coded_picture_buffer_count; index++) {
        if (reader.has_overrun())
            return;
        reader.read_unsigned_exp_golomb(); // bit_rate_value_minus1
        reader.read_unsigned_exp_golomb(); // cpb_size_value_minus1
        reader.skip_bits(1);               // cbr_flag
    }
    reader.skip_bits(5); // initial_cpb_removal_delay_length_minus1
    reader.skip_bits(5); // cpb_removal_delay_length_minus1
    reader.skip_bits(5); // dpb_output_delay_length_minus1
    reader.skip_bits(5); // time_offset_length
}

static constexpr u8 MAXIMUM_REORDER_FRAME_COUNT = 16;

// ITU-T H.264 (08/2024), E.1.1: vui_parameters(), through max_num_reorder_frames.
// Returns the max_num_reorder_frames the bitstream restriction states, if it states one at all.
static Optional<u32> parse_stated_max_reorder_frame_count(Codecs::RBSPBitReader& reader)
{
    static constexpr u8 EXTENDED_SAMPLE_ASPECT_RATIO = 255;

    if (reader.read_bit()) { // aspect_ratio_info_present_flag
        auto aspect_ratio_idc = reader.read_bits<u8>(8);
        if (aspect_ratio_idc == EXTENDED_SAMPLE_ASPECT_RATIO) {
            reader.skip_bits(16); // sar_width
            reader.skip_bits(16); // sar_height
        }
    }
    if (reader.read_bit())       // overscan_info_present_flag
        reader.skip_bits(1);     // overscan_appropriate_flag
    if (reader.read_bit()) {     // video_signal_type_present_flag
        reader.skip_bits(3);     // video_format
        reader.skip_bits(1);     // video_full_range_flag
        if (reader.read_bit()) { // colour_description_present_flag
            reader.skip_bits(8); // colour_primaries
            reader.skip_bits(8); // transfer_characteristics
            reader.skip_bits(8); // matrix_coefficients
        }
    }
    if (reader.read_bit()) {               // chroma_loc_info_present_flag
        reader.read_unsigned_exp_golomb(); // chroma_sample_loc_type_top_field
        reader.read_unsigned_exp_golomb(); // chroma_sample_loc_type_bottom_field
    }
    if (reader.read_bit()) {  // timing_info_present_flag
        reader.skip_bits(32); // num_units_in_tick
        reader.skip_bits(32); // time_scale
        reader.skip_bits(1);  // fixed_frame_rate_flag
    }

    auto has_nal_hrd_parameters = reader.read_bit();
    if (has_nal_hrd_parameters)
        skip_hrd_parameters(reader);
    auto has_vcl_hrd_parameters = reader.read_bit();
    if (has_vcl_hrd_parameters)
        skip_hrd_parameters(reader);
    if (has_nal_hrd_parameters || has_vcl_hrd_parameters)
        reader.skip_bits(1); // low_delay_hrd_flag
    reader.skip_bits(1);     // pic_struct_present_flag

    if (!reader.read_bit()) // bitstream_restriction_flag
        return {};
    reader.skip_bits(1);                      // motion_vectors_over_pic_boundaries_flag
    reader.read_unsigned_exp_golomb();        // max_bytes_per_pic_denom
    reader.read_unsigned_exp_golomb();        // max_bits_per_mb_denom
    reader.read_unsigned_exp_golomb();        // log2_max_mv_length_horizontal
    reader.read_unsigned_exp_golomb();        // log2_max_mv_length_vertical
    return reader.read_unsigned_exp_golomb(); // max_num_reorder_frames
}

static u8 max_reorder_frame_count_for_level(u8 level_idc, u32 macroblocks_per_frame)
{
    auto max_decoded_picture_buffer_macroblocks = [level_idc]() -> Optional<u32> {
        switch (level_idc) {
        case 10:
            return 396;
        case 11:
            return 900;
        case 12:
        case 13:
        case 20:
            return 2376;
        case 21:
            return 4752;
        case 22:
        case 30:
            return 8100;
        case 31:
            return 18000;
        case 32:
            return 20480;
        case 40:
        case 41:
            return 32768;
        case 42:
            return 34816;
        case 50:
            return 110400;
        case 51:
        case 52:
            return 184320;
        case 60:
        case 61:
        case 62:
            return 696320;
        default:
            return {};
        }
    }();

    if (!max_decoded_picture_buffer_macroblocks.has_value() || macroblocks_per_frame == 0)
        return MAXIMUM_REORDER_FRAME_COUNT;
    return static_cast<u8>(min(*max_decoded_picture_buffer_macroblocks / macroblocks_per_frame, static_cast<u32>(MAXIMUM_REORDER_FRAME_COUNT)));
}

// ITU-T H.264 (08/2024), 7.3.2.1.1: seq_parameter_set_data().
Optional<H264::SequenceParameterSet> H264::parse_sequence_parameter_set(ReadonlyBytes nal_unit)
{
    if (nal_unit.is_empty() || (nal_unit[0] & NAL_UNIT_TYPE_MASK) != SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE || (nal_unit[0] & 0x80) != 0)
        return {};
    Codecs::RBSPBitReader reader { nal_unit.slice(NAL_UNIT_HEADER_SIZE) };
    auto profile_idc = reader.read_bits<u8>(8);
    reader.skip_bits(3); // constraint_set0_flag through constraint_set2_flag
    auto constraint_set3_flag = reader.read_bit();
    reader.skip_bits(2); // constraint_set4_flag through constraint_set5_flag
    reader.skip_bits(2); // reserved_zero_2bits
    auto level_idc = reader.read_bits<u8>(8);
    auto seq_parameter_set_id = reader.read_unsigned_exp_golomb();
    // ITU-T H.264 (08/2024), 7.4.2.1.1: seq_parameter_set_id is in the range 0 to 31.
    if (reader.has_overrun() || seq_parameter_set_id > 31)
        return {};

    if (profile_has_chroma_format_fields(profile_idc)) {
        auto chroma_format_idc = reader.read_unsigned_exp_golomb();
        if (chroma_format_idc == 3)
            reader.skip_bits(1);           // separate_colour_plane_flag
        reader.read_unsigned_exp_golomb(); // bit_depth_luma_minus8
        reader.read_unsigned_exp_golomb(); // bit_depth_chroma_minus8
        reader.skip_bits(1);               // qpprime_y_zero_transform_bypass_flag
        if (reader.read_bit()) {           // seq_scaling_matrix_present_flag
            auto scaling_list_count = chroma_format_idc != 3 ? 8 : 12;
            for (auto index = 0; index < scaling_list_count; index++) {
                if (reader.read_bit()) // seq_scaling_list_present_flag
                    skip_scaling_list(reader, index < 6 ? 16 : 64);
            }
        }
    }

    reader.read_unsigned_exp_golomb(); // log2_max_frame_num_minus4
    auto picture_order_count_type = reader.read_unsigned_exp_golomb();
    if (picture_order_count_type == 0) {
        reader.read_unsigned_exp_golomb(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (picture_order_count_type == 1) {
        reader.skip_bits(1);             // delta_pic_order_always_zero_flag
        reader.read_signed_exp_golomb(); // offset_for_non_ref_pic
        reader.read_signed_exp_golomb(); // offset_for_top_to_bottom_field
        auto cycle_length = reader.read_unsigned_exp_golomb();
        for (u32 index = 0; index < cycle_length; index++) {
            if (reader.has_overrun())
                return {};
            reader.read_signed_exp_golomb(); // offset_for_ref_frame
        }
    }

    reader.read_unsigned_exp_golomb(); // max_num_ref_frames
    reader.skip_bits(1);               // gaps_in_frame_num_value_allowed_flag
    auto width_in_macroblocks = reader.read_unsigned_exp_golomb() + 1;
    auto height_in_map_units = reader.read_unsigned_exp_golomb() + 1;
    auto is_frame_only = reader.read_bit();
    if (!is_frame_only)
        reader.skip_bits(1);               // mb_adaptive_frame_field_flag
    reader.skip_bits(1);                   // direct_8x8_inference_flag
    if (reader.read_bit()) {               // frame_cropping_flag
        reader.read_unsigned_exp_golomb(); // frame_crop_left_offset
        reader.read_unsigned_exp_golomb(); // frame_crop_right_offset
        reader.read_unsigned_exp_golomb(); // frame_crop_top_offset
        reader.read_unsigned_exp_golomb(); // frame_crop_bottom_offset
    }

    Optional<u32> stated_count;
    if (reader.read_bit()) // vui_parameters_present_flag
        stated_count = parse_stated_max_reorder_frame_count(reader);

    if (reader.has_overrun())
        return {};
    // ITU-T H.264 (08/2024), E.2.1: Infer absent max_num_reorder_frames as zero for these
    // profiles with constraint_set3_flag set, and as MaxDpbFrames otherwise.
    SequenceParameterSet parameter_set { .seq_parameter_set_id = static_cast<u8>(seq_parameter_set_id), .max_num_reorder_frames = 0 };
    if (stated_count.has_value()) {
        parameter_set.max_num_reorder_frames = static_cast<u8>(min(*stated_count, static_cast<u32>(MAXIMUM_REORDER_FRAME_COUNT)));
    } else if (!(constraint_set3_flag && first_is_one_of(profile_idc, 44, 86, 100, 110, 122, 244))) {
        auto height_in_macroblocks = height_in_map_units * (is_frame_only ? 1 : 2);
        parameter_set.max_num_reorder_frames = max_reorder_frame_count_for_level(level_idc, width_in_macroblocks * height_in_macroblocks);
    }
    return parameter_set;
}

// ITU-T H.264 (08/2024), 7.3.2.2: pic_parameter_set_rbsp(), through seq_parameter_set_id.
Optional<H264::PictureParameterSet> H264::parse_picture_parameter_set(ReadonlyBytes nal_unit)
{
    if (nal_unit.is_empty() || (nal_unit[0] & NAL_UNIT_TYPE_MASK) != PICTURE_PARAMETER_SET_NAL_UNIT_TYPE || (nal_unit[0] & 0x80) != 0)
        return {};
    Codecs::RBSPBitReader reader { nal_unit.slice(NAL_UNIT_HEADER_SIZE) };
    auto pic_parameter_set_id = reader.read_unsigned_exp_golomb();
    auto seq_parameter_set_id = reader.read_unsigned_exp_golomb();
    // ITU-T H.264 (08/2024), 7.4.2.2: pic_parameter_set_id is in 0 to 255, seq_parameter_set_id in 0 to 31.
    if (reader.has_overrun() || pic_parameter_set_id > 255 || seq_parameter_set_id > 31)
        return {};
    return PictureParameterSet {
        .pic_parameter_set_id = static_cast<u8>(pic_parameter_set_id),
        .seq_parameter_set_id = static_cast<u8>(seq_parameter_set_id),
    };
}

H264::ParameterSetStore::ParameterSetStore()
{
    m_sequence_indices.fill(ABSENT);
    m_picture_indices.fill(ABSENT);
}

H264::ParameterSetStore::StoredSPS const* H264::ParameterSetStore::sequence_parameter_set(u8 id) const
{
    if (id >= m_sequence_indices.size() || m_sequence_indices[id] == ABSENT)
        return nullptr;
    return &m_sequence[m_sequence_indices[id]];
}

H264::ParameterSetStore::StoredPPS const* H264::ParameterSetStore::picture_parameter_set(u8 id) const
{
    if (m_picture_indices[id] == ABSENT)
        return nullptr;
    return &m_picture[m_picture_indices[id]];
}

ErrorOr<bool> H264::ParameterSetStore::apply_nal_unit(ReadonlyBytes nal_unit)
{
    if (nal_unit.is_empty())
        return Error::from_string_literal("Empty H.264 NAL unit");

    // ITU-T H.264 (08/2024), 7.4.1.2.1: Replace only the matching ID; retain other definitions.
    // Append new IDs without removing or reordering entries, so indices survive vector growth.
    auto store = [](auto& entries, auto& indices, u8 id, auto const& parameters, ReadonlyBytes nal_unit) -> ErrorOr<bool> {
        using Entry = typename RemoveReference<decltype(entries)>::ValueType;

        if (indices[id] != ABSENT) {
            auto& stored_data = entries[indices[id]];
            if (stored_data.nal_unit.bytes() == nal_unit)
                return false;
            auto bytes = TRY(ByteBuffer::copy(nal_unit));
            stored_data.parameters = parameters;
            stored_data.nal_unit = move(bytes);
            return true;
        }

        auto bytes = TRY(ByteBuffer::copy(nal_unit));
        auto index = static_cast<u16>(entries.size());
        TRY(entries.try_append(Entry { parameters, move(bytes) }));
        indices[id] = index;
        return true;
    };

    switch (nal_unit[0] & NAL_UNIT_TYPE_MASK) {
    case SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE: {
        auto parameters = parse_sequence_parameter_set(nal_unit);
        if (!parameters.has_value())
            return Error::from_string_literal("Invalid H.264 sequence parameter set");
        return store(m_sequence, m_sequence_indices, parameters->seq_parameter_set_id, *parameters, nal_unit);
    }
    case PICTURE_PARAMETER_SET_NAL_UNIT_TYPE: {
        auto parameters = parse_picture_parameter_set(nal_unit);
        if (!parameters.has_value())
            return Error::from_string_literal("Invalid H.264 picture parameter set");
        return store(m_picture, m_picture_indices, parameters->pic_parameter_set_id, *parameters, nal_unit);
    }
    default:
        return false;
    }
}

Optional<H264::ParameterSets> H264::parse_parameter_sets_from_configuration_record(ReadonlyBytes record)
{
    BitReader reader { record };
    if (reader.read_bits<u8>(8) != 1) // configurationVersion
        return {};

    reader.skip_bits(8); // AVCProfileIndication
    reader.skip_bits(8); // profile_compatibility
    reader.skip_bits(8); // AVCLevelIndication
    reader.skip_bits(6); // Reserved

    ParameterSets parameter_sets;
    parameter_sets.nal_unit_length_size = reader.read_bits<u8>(2) + 1;
    // ISO/IEC 14496-15 does not define a three-byte length prefix.
    if (parameter_sets.nal_unit_length_size == 3)
        return {};
    reader.skip_bits(3); // Reserved

    auto sequence_parameter_set_count = reader.read_bits<u8>(5);
    if (reader.has_overrun())
        return {};

    VERIFY(reader.bit_position() % 8 == 0);
    auto remaining_data = record.slice(reader.bit_position() / 8);

    auto read_into = [&](Vector<ReadonlyBytes, 2>& sets, u8 count, u8 nal_unit_type) {
        NALUnitIterator iterator { remaining_data, CONFIGURATION_RECORD_NAL_UNIT_LENGTH_SIZE };
        for (u8 index = 0; index < count; index++) {
            auto nal_unit = iterator.next();
            if (!nal_unit.has_value())
                return false;
            if (((*nal_unit)[0] & NAL_UNIT_TYPE_MASK) == nal_unit_type)
                sets.append(*nal_unit);
        }
        remaining_data = iterator.remaining_data();
        return true;
    };

    if (!read_into(parameter_sets.sequence, sequence_parameter_set_count, SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE))
        return {};

    // A record may end after its sequence parameter sets, but a stated count of picture sets has to be readable.
    if (!remaining_data.is_empty()) {
        auto picture_parameter_set_count = remaining_data[0];
        remaining_data = remaining_data.slice(1);
        if (!read_into(parameter_sets.picture, picture_parameter_set_count, PICTURE_PARAMETER_SET_NAL_UNIT_TYPE))
            return {};
    }

    return parameter_sets;
}

// profile_idc 66, Baseline and Constrained Baseline.
static constexpr Array<u8, 42> BASELINE_CONFIGURATION_RECORD {
    0x01, 0x42, 0xc0, 0x28, 0xff, 0xe1, 0x00, 0x1a, 0x67, 0x42, 0xc0, 0x28, 0xd9, 0x00, 0x78, 0x02, 0x27, 0xe5,
    0xc0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x3c, 0x60, 0xc9, 0x20, 0x01, 0x00,
    0x05, 0x68, 0xcb, 0x83, 0xcb, 0x20
};

// profile_idc 77, Main.
static constexpr Array<u8, 42> MAIN_CONFIGURATION_RECORD {
    0x01, 0x4d, 0x40, 0x28, 0xff, 0xe1, 0x00, 0x1a, 0x67, 0x4d, 0x40, 0x28, 0xec, 0xa0, 0x3c, 0x01, 0x13, 0xf2,
    0xe0, 0x22, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x64, 0x1e, 0x30, 0x63, 0x2c, 0x01, 0x00,
    0x05, 0x68, 0xeb, 0xe3, 0xcb, 0x20
};

// profile_idc 100, High.
static constexpr Array<u8, 48> HIGH_CONFIGURATION_RECORD {
    0x01, 0x64, 0x00, 0x28, 0xff, 0xe1, 0x00, 0x1b, 0x67, 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0x78, 0x02, 0x27,
    0xe5, 0xc0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x3c, 0x60, 0xc6, 0x58, 0x01,
    0x00, 0x06, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0xfd, 0xf8, 0xf8, 0x00
};

// profile_idc 110, High 10.
static constexpr Array<u8, 48> HIGH_10_CONFIGURATION_RECORD {
    0x01, 0x6e, 0x00, 0x28, 0xff, 0xe1, 0x00, 0x1b, 0x67, 0x6e, 0x00, 0x28, 0xa6, 0xcd, 0x94, 0x07, 0x80, 0x22,
    0x7e, 0x5c, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc6, 0x0c, 0x65, 0x80, 0x01,
    0x00, 0x06, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0xfd, 0xfa, 0xfa, 0x00
};

// profile_idc 122, High 4:2:2.
static constexpr Array<u8, 49> HIGH_4_2_2_CONFIGURATION_RECORD {
    0x01, 0x7a, 0x00, 0x28, 0xff, 0xe1, 0x00, 0x1c, 0x67, 0x7a, 0x00, 0x28, 0xb6, 0xcd, 0x94, 0x07, 0x80, 0x22,
    0x7e, 0x27, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60,
    0x01, 0x00, 0x06, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0xfe, 0xfa, 0xfa, 0x00
};

// profile_idc 244, High 4:4:4 Predictive.
static constexpr Array<u8, 49> HIGH_4_4_4_CONFIGURATION_RECORD {
    0x01, 0xf4, 0x00, 0x28, 0xff, 0xe1, 0x00, 0x1c, 0x67, 0xf4, 0x00, 0x28, 0x90, 0xd9, 0xb2, 0x80, 0xf0, 0x04,
    0x4f, 0xc4, 0xe0, 0x22, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x64, 0x1e, 0x30, 0x63, 0x2c,
    0x01, 0x00, 0x06, 0x68, 0xeb, 0xe3, 0xc4, 0x48, 0x44, 0xff, 0xfa, 0xfa, 0x00
};

// ITU-T H.264 (08/2024), 7.4.2.1.1: "constraint_set0_flag equal to 1 indicates that the coded video sequence obeys
// all constraints specified in clause A.2.1", the Baseline profile, with constraint_set1_flag saying the same of
// A.2.2, Main. A stream stating so can be decoded by a decoder for that profile, whatever profile it names.
Optional<H264::Profile> H264::Parameters::profile() const
{
    static constexpr u8 CONSTRAINT_SET0_FLAG = 0x80;
    static constexpr u8 CONSTRAINT_SET1_FLAG = 0x40;

    Optional<Profile> profile;
    switch (profile_idc) {
    case 66:
        profile = Profile::Baseline;
        break;
    case 77:
        profile = Profile::Main;
        break;
    case 100:
        profile = Profile::High;
        break;
    case 110:
        profile = Profile::High10;
        break;
    case 122:
        profile = Profile::High422;
        break;
    case 244:
        profile = Profile::High444;
        break;
    default:
        break;
    }

    // NB: constraint_set2_flag names the Extended profile, which no record stands in for, so it cannot narrow this.
    if ((constraint_set_flags & CONSTRAINT_SET1_FLAG) != 0 && (!profile.has_value() || *profile > Profile::Main))
        profile = Profile::Main;
    if ((constraint_set_flags & CONSTRAINT_SET0_FLAG) != 0 && (!profile.has_value() || *profile > Profile::Baseline))
        profile = Profile::Baseline;
    return profile;
}

ReadonlyBytes H264::representative_configuration_record_for_profile(Profile profile)
{
    switch (profile) {
    case Profile::Baseline:
        return BASELINE_CONFIGURATION_RECORD;
    case Profile::Main:
        return MAIN_CONFIGURATION_RECORD;
    case Profile::High:
        return HIGH_CONFIGURATION_RECORD;
    case Profile::High10:
        return HIGH_10_CONFIGURATION_RECORD;
    case Profile::High422:
        return HIGH_4_2_2_CONFIGURATION_RECORD;
    case Profile::High444:
        return HIGH_4_4_4_CONFIGURATION_RECORD;
    }
    VERIFY_NOT_REACHED();
}

H264::Parameters H264::canonical_parameters_for_profile(Profile profile)
{
    auto profile_idc = [profile]() -> u8 {
        switch (profile) {
        case Profile::Baseline:
            return 66;
        case Profile::Main:
            return 77;
        case Profile::High:
            return 100;
        case Profile::High10:
            return 110;
        case Profile::High422:
            return 122;
        case Profile::High444:
            return 244;
        }
        VERIFY_NOT_REACHED();
    }();
    return Parameters { .profile_idc = profile_idc, .constraint_set_flags = 0, .level_idc = 0 };
}

}
