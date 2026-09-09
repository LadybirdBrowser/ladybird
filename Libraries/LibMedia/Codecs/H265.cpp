/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/BitReader.h>
#include <LibMedia/Codecs/CodecString.h>
#include <LibMedia/Codecs/H265.h>
#include <LibMedia/Codecs/NALUnit.h>
#include <LibMedia/Codecs/RBSPBitReader.h>

namespace Media::Codecs {

static constexpr size_t NAL_UNIT_HEADER_SIZE = 2;
static constexpr u8 VIDEO_PARAMETER_SET_NAL_UNIT_TYPE = 32;
static constexpr u8 SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE = 33;
static constexpr u8 PICTURE_PARAMETER_SET_NAL_UNIT_TYPE = 34;
static constexpr u8 LAST_CODED_SLICE_NAL_UNIT_TYPE = 31;
static constexpr u8 FIRST_IRAP_NAL_UNIT_TYPE = 16;
static constexpr u8 MAXIMUM_REORDER_FRAME_COUNT = 16;

Optional<H265::Parameters> H265::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};

    Parameters parameters {};
    if (lexer.next_is(is_any_of("ABC"sv)))
        parameters.profile_space = lexer.consume() - 'A' + 1;

    auto profile_idc = lexer.consume_while(is_ascii_digit);
    if (profile_idc.is_empty() || profile_idc.length() > 2)
        return {};
    auto maybe_profile_idc = profile_idc.to_number<u8>(TrimWhitespace::No);
    if (!maybe_profile_idc.has_value() || *maybe_profile_idc > 31)
        return {};
    parameters.profile_idc = *maybe_profile_idc;

    if (!lexer.consume_specific('.'))
        return {};

    auto profile_compatibility_flags = lexer.consume_while(is_ascii_hex_digit);
    if (profile_compatibility_flags.is_empty() || profile_compatibility_flags.length() > 8)
        return {};
    auto maybe_profile_compatibility_flags = profile_compatibility_flags.to_number<u32>(TrimWhitespace::No, 16);
    if (!maybe_profile_compatibility_flags.has_value())
        return {};
    parameters.profile_compatibility_flags = *maybe_profile_compatibility_flags;

    if (!lexer.consume_specific('.'))
        return {};

    if (!lexer.next_is(is_any_of("LH"sv)))
        return {};
    parameters.tier_flag = lexer.consume() == 'H';

    auto level_idc = lexer.consume_while(is_ascii_digit);
    if (level_idc.is_empty() || level_idc.length() > 3)
        return {};
    auto maybe_level_idc = level_idc.to_number<u8>(TrimWhitespace::No);
    if (!maybe_level_idc.has_value())
        return {};
    parameters.level_idc = *maybe_level_idc;

    for (size_t index = 0; lexer.consume_specific('.'); index++) {
        if (index == parameters.constraint_indicator_flags.size())
            return {};
        auto digits = lexer.consume_while(is_ascii_hex_digit);
        if (digits.is_empty() || digits.length() > 2)
            return {};
        auto flag = digits.to_number<u8>(TrimWhitespace::No, 16);
        if (!flag.has_value())
            return {};
        parameters.constraint_indicator_flags[index] = *flag;
    }

    if (!lexer.is_eof())
        return {};
    return parameters;
}

// Parameters holds the flags indexed by profile, where general_profile_compatibility_flag[i] is bit i.
static u32 reverse_profile_compatibility_flag_bits(u32 flags)
{
    u32 reversed = 0;
    for (size_t bit = 0; bit < 32; bit++)
        reversed |= ((flags >> bit) & 1) << (31 - bit);
    return reversed;
}

Optional<H265::Parameters> H265::parse_configuration_record(ReadonlyBytes record)
{
    BitReader reader { record };

    auto version = reader.read_bits<u8>(8);
    if (version != 1)
        return {};

    Parameters parameters {};
    parameters.profile_space = reader.read_bits<u8>(2);
    parameters.tier_flag = reader.read_bit();
    parameters.profile_idc = reader.read_bits<u8>(5);
    parameters.profile_compatibility_flags = reverse_profile_compatibility_flag_bits(reader.read_bits<u32>(32));
    for (auto& constraint_indicator_flag : parameters.constraint_indicator_flags)
        constraint_indicator_flag = reader.read_bits<u8>(8);
    parameters.level_idc = reader.read_bits<u8>(8);

    if (reader.has_overrun())
        return {};
    return parameters;
}

// ITU-T H.265 (07/2024), 7.3.1.2: nal_unit_header().
Optional<H265::NALUnitHeader> H265::parse_nal_unit_header(ReadonlyBytes nal_unit)
{
    if (nal_unit.size() < NAL_UNIT_HEADER_SIZE)
        return {};
    if ((nal_unit[0] & 0x80) != 0) // forbidden_zero_bit
        return {};
    auto temporal_id_plus1 = nal_unit[1] & 0x07;
    // ITU-T H.265 (07/2024), 7.4.2.2: nuh_temporal_id_plus1 shall not be equal to 0.
    if (temporal_id_plus1 == 0)
        return {};

    return NALUnitHeader {
        .nal_unit_type = static_cast<u8>((nal_unit[0] >> 1) & 0x3f),
        .nuh_layer_id = static_cast<u8>(((nal_unit[0] & 0x01) << 5) | (nal_unit[1] >> 3)),
        .temporal_id = static_cast<u8>(temporal_id_plus1 - 1),
    };
}

// ITU-T H.265 (07/2024), Table 7-1: nal_unit_type 0 through 31 carry coded slice segments.
bool H265::is_coded_slice(NALUnitHeader const& header)
{
    return header.nal_unit_type <= LAST_CODED_SLICE_NAL_UNIT_TYPE;
}

// ITU-T H.265 (07/2024), 7.4.2.2: the even types below the IRAP range are the sub-layer non-reference pictures.
bool H265::is_sub_layer_non_reference(NALUnitHeader const& header)
{
    return header.nal_unit_type < FIRST_IRAP_NAL_UNIT_TYPE && header.nal_unit_type % 2 == 0;
}

// ITU-T H.265 (07/2024), 7.3.3: the spec notes that the constraint flag branches do not affect the structure's
// width, so reaching what follows only takes counting bits.
static void skip_profile_tier_level(RBSPBitReader& reader, u8 max_sub_layers_minus1)
{
    static constexpr size_t GENERAL_PROFILE_TIER_LEVEL_BIT_COUNT = 96;
    static constexpr size_t SUB_LAYER_PROFILE_BIT_COUNT = 88;
    static constexpr size_t SUB_LAYER_LEVEL_BIT_COUNT = 8;

    reader.skip_bits(GENERAL_PROFILE_TIER_LEVEL_BIT_COUNT);
    if (max_sub_layers_minus1 == 0)
        return;

    Array<bool, 7> profile_present {};
    Array<bool, 7> level_present {};
    for (u8 index = 0; index < max_sub_layers_minus1; index++) {
        profile_present[index] = reader.read_bit();
        level_present[index] = reader.read_bit();
    }
    reader.skip_bits(2 * static_cast<size_t>(8u - max_sub_layers_minus1)); // reserved_zero_2bits

    for (u8 index = 0; index < max_sub_layers_minus1; index++) {
        if (profile_present[index])
            reader.skip_bits(SUB_LAYER_PROFILE_BIT_COUNT);
        if (level_present[index])
            reader.skip_bits(SUB_LAYER_LEVEL_BIT_COUNT);
    }
}

// ITU-T H.265 (07/2024), 7.3.2.1: video_parameter_set_rbsp(), whose first element is all we track.
Optional<H265::VideoParameterSet> H265::parse_video_parameter_set(ReadonlyBytes nal_unit)
{
    auto header = parse_nal_unit_header(nal_unit);
    if (!header.has_value() || header->nal_unit_type != VIDEO_PARAMETER_SET_NAL_UNIT_TYPE)
        return {};

    RBSPBitReader reader { nal_unit.slice(NAL_UNIT_HEADER_SIZE) };
    auto vps_video_parameter_set_id = reader.read_bits<u8>(4);
    if (reader.has_overrun())
        return {};
    return VideoParameterSet { .vps_video_parameter_set_id = vps_video_parameter_set_id };
}

// ITU-T H.265 (07/2024), 7.3.2.2.1: seq_parameter_set_rbsp(), through the sub-layer ordering information.
Optional<H265::SequenceParameterSet> H265::parse_sequence_parameter_set(ReadonlyBytes nal_unit)
{
    auto header = parse_nal_unit_header(nal_unit);
    if (!header.has_value() || header->nal_unit_type != SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE)
        return {};

    RBSPBitReader reader { nal_unit.slice(NAL_UNIT_HEADER_SIZE) };
    auto sps_video_parameter_set_id = reader.read_bits<u8>(4);
    auto sps_max_sub_layers_minus1 = reader.read_bits<u8>(3);
    reader.skip_bits(1); // sps_temporal_id_nesting_flag
    // ITU-T H.265 (07/2024), 7.4.3.2.1: sps_max_sub_layers_minus1 is in the range 0 to 6.
    if (reader.has_overrun() || sps_max_sub_layers_minus1 > 6)
        return {};

    skip_profile_tier_level(reader, sps_max_sub_layers_minus1);

    auto sps_seq_parameter_set_id = reader.read_unsigned_exp_golomb();
    auto chroma_format_idc = reader.read_unsigned_exp_golomb();
    if (chroma_format_idc == 3)
        reader.skip_bits(1);               // separate_colour_plane_flag
    reader.read_unsigned_exp_golomb();     // pic_width_in_luma_samples
    reader.read_unsigned_exp_golomb();     // pic_height_in_luma_samples
    if (reader.read_bit()) {               // conformance_window_flag
        reader.read_unsigned_exp_golomb(); // conf_win_left_offset
        reader.read_unsigned_exp_golomb(); // conf_win_right_offset
        reader.read_unsigned_exp_golomb(); // conf_win_top_offset
        reader.read_unsigned_exp_golomb(); // conf_win_bottom_offset
    }
    auto bit_depth_luma_minus8 = reader.read_unsigned_exp_golomb();
    reader.read_unsigned_exp_golomb(); // bit_depth_chroma_minus8
    reader.read_unsigned_exp_golomb(); // log2_max_pic_order_cnt_lsb_minus4

    // ITU-T H.265 (07/2024), 7.4.3.2.1: the counts absent below the highest sub-layer are inferred from it, so the
    // last one coded is the one that bounds the whole stream.
    auto sub_layer_ordering_info_is_present = reader.read_bit();
    u32 sps_max_num_reorder_pics = 0;
    for (u8 index = sub_layer_ordering_info_is_present ? 0 : sps_max_sub_layers_minus1; index <= sps_max_sub_layers_minus1; index++) {
        reader.read_unsigned_exp_golomb(); // sps_max_dec_pic_buffering_minus1
        sps_max_num_reorder_pics = reader.read_unsigned_exp_golomb();
        reader.read_unsigned_exp_golomb(); // sps_max_latency_increase_plus1
    }

    // ITU-T H.265 (07/2024), 7.4.3.2.1: sps_seq_parameter_set_id is in 0 to 15, bit_depth_luma_minus8 in 0 to 8.
    if (reader.has_overrun() || sps_seq_parameter_set_id > 15 || bit_depth_luma_minus8 > 8)
        return {};

    return SequenceParameterSet {
        .sps_seq_parameter_set_id = static_cast<u8>(sps_seq_parameter_set_id),
        .sps_video_parameter_set_id = sps_video_parameter_set_id,
        .sps_max_sub_layers_minus1 = sps_max_sub_layers_minus1,
        .sps_max_num_reorder_pics = static_cast<u8>(min(sps_max_num_reorder_pics, static_cast<u32>(MAXIMUM_REORDER_FRAME_COUNT))),
        .bit_depth_luma = static_cast<u8>(bit_depth_luma_minus8 + 8),
    };
}

// ITU-T H.265 (07/2024), 7.3.2.3.1: pic_parameter_set_rbsp(), whose two IDs open the payload.
Optional<H265::PictureParameterSet> H265::parse_picture_parameter_set(ReadonlyBytes nal_unit)
{
    auto header = parse_nal_unit_header(nal_unit);
    if (!header.has_value() || header->nal_unit_type != PICTURE_PARAMETER_SET_NAL_UNIT_TYPE)
        return {};

    RBSPBitReader reader { nal_unit.slice(NAL_UNIT_HEADER_SIZE) };
    auto pps_pic_parameter_set_id = reader.read_unsigned_exp_golomb();
    auto pps_seq_parameter_set_id = reader.read_unsigned_exp_golomb();
    // ITU-T H.265 (07/2024), 7.4.3.3.1: pps_pic_parameter_set_id is in 0 to 63, pps_seq_parameter_set_id in 0 to 15.
    if (reader.has_overrun() || pps_pic_parameter_set_id > 63 || pps_seq_parameter_set_id > 15)
        return {};

    return PictureParameterSet {
        .pps_pic_parameter_set_id = static_cast<u8>(pps_pic_parameter_set_id),
        .pps_seq_parameter_set_id = static_cast<u8>(pps_seq_parameter_set_id),
    };
}

H265::ParameterSetStore::ParameterSetStore()
{
    m_video_indices.fill(ABSENT);
    m_sequence_indices.fill(ABSENT);
    m_picture_indices.fill(ABSENT);
}

H265::ParameterSetStore::StoredVPS const* H265::ParameterSetStore::video_parameter_set(u8 id) const
{
    if (id >= m_video_indices.size() || m_video_indices[id] == ABSENT)
        return nullptr;
    return &m_video[m_video_indices[id]];
}

H265::ParameterSetStore::StoredSPS const* H265::ParameterSetStore::sequence_parameter_set(u8 id) const
{
    if (id >= m_sequence_indices.size() || m_sequence_indices[id] == ABSENT)
        return nullptr;
    return &m_sequence[m_sequence_indices[id]];
}

H265::ParameterSetStore::StoredPPS const* H265::ParameterSetStore::picture_parameter_set(u8 id) const
{
    if (id >= m_picture_indices.size() || m_picture_indices[id] == ABSENT)
        return nullptr;
    return &m_picture[m_picture_indices[id]];
}

ErrorOr<bool> H265::ParameterSetStore::apply_nal_unit(ReadonlyBytes nal_unit)
{
    auto header = parse_nal_unit_header(nal_unit);
    if (!header.has_value())
        return Error::from_string_literal("Invalid H.265 NAL unit header");
    // An enhancement layer's sets describe a stream this decoder is not configured for.
    if (header->nuh_layer_id != 0)
        return false;

    // ITU-T H.265 (07/2024), 7.4.2.4.1: Replace only the matching ID; retain other definitions.
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

    switch (header->nal_unit_type) {
    case VIDEO_PARAMETER_SET_NAL_UNIT_TYPE: {
        auto parameters = parse_video_parameter_set(nal_unit);
        if (!parameters.has_value())
            return Error::from_string_literal("Invalid H.265 video parameter set");
        return store(m_video, m_video_indices, parameters->vps_video_parameter_set_id, *parameters, nal_unit);
    }
    case SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE: {
        auto parameters = parse_sequence_parameter_set(nal_unit);
        if (!parameters.has_value())
            return Error::from_string_literal("Invalid H.265 sequence parameter set");
        return store(m_sequence, m_sequence_indices, parameters->sps_seq_parameter_set_id, *parameters, nal_unit);
    }
    case PICTURE_PARAMETER_SET_NAL_UNIT_TYPE: {
        auto parameters = parse_picture_parameter_set(nal_unit);
        if (!parameters.has_value())
            return Error::from_string_literal("Invalid H.265 picture parameter set");
        return store(m_picture, m_picture_indices, parameters->pps_pic_parameter_set_id, *parameters, nal_unit);
    }
    default:
        return false;
    }
}

// ISO/IEC 14496-15: HEVCDecoderConfigurationRecord, whose parameter sets are grouped into per-type arrays rather
// than the two counted runs an avcC carries.
Optional<H265::ParameterSets> H265::parse_parameter_sets_from_configuration_record(ReadonlyBytes record)
{
    // configurationVersion through numOfArrays.
    static constexpr size_t FIXED_HEADER_SIZE = 23;
    static constexpr size_t ARRAY_HEADER_SIZE = 3;
    static constexpr size_t LENGTH_SIZE_OFFSET = 21;
    static constexpr size_t ARRAY_COUNT_OFFSET = 22;

    if (record.size() < FIXED_HEADER_SIZE)
        return {};
    if (record[0] > 1) // configurationVersion
        return {};

    ParameterSets parameter_sets;
    parameter_sets.nal_unit_length_size = (record[LENGTH_SIZE_OFFSET] & 0x03) + 1;
    // ISO/IEC 14496-15 does not define a three-byte length prefix.
    if (parameter_sets.nal_unit_length_size == 3)
        return {};

    auto array_count = record[ARRAY_COUNT_OFFSET];
    auto remaining_data = record.slice(FIXED_HEADER_SIZE);

    for (u8 array_index = 0; array_index < array_count; array_index++) {
        if (remaining_data.size() < ARRAY_HEADER_SIZE)
            return parameter_sets;
        // The array states a NAL unit type of its own, but each unit's header is what the sets are keyed by.
        auto nal_unit_count = static_cast<u16>((remaining_data[1] << 8) | remaining_data[2]);
        remaining_data = remaining_data.slice(ARRAY_HEADER_SIZE);

        NALUnitIterator iterator { remaining_data, CONFIGURATION_RECORD_NAL_UNIT_LENGTH_SIZE };
        for (u16 index = 0; index < nal_unit_count; index++) {
            auto nal_unit = iterator.next();
            if (!nal_unit.has_value())
                return parameter_sets;

            auto header = parse_nal_unit_header(*nal_unit);
            if (!header.has_value() || header->nuh_layer_id != 0)
                continue;
            switch (header->nal_unit_type) {
            case VIDEO_PARAMETER_SET_NAL_UNIT_TYPE:
                parameter_sets.video.append(*nal_unit);
                break;
            case SEQUENCE_PARAMETER_SET_NAL_UNIT_TYPE:
                parameter_sets.sequence.append(*nal_unit);
                break;
            case PICTURE_PARAMETER_SET_NAL_UNIT_TYPE:
                parameter_sets.picture.append(*nal_unit);
                break;
            default:
                break;
            }
        }
        remaining_data = iterator.remaining_data();
    }

    return parameter_sets;
}

// general_profile_idc 1, Main.
static constexpr Array<u8, 111> MAIN_CONFIGURATION_RECORD {
    0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xfd, 0xf8,
    0xf8, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x18, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0x95, 0x94, 0x09, 0xa1, 0x00,
    0x01, 0x00, 0x2b, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x03, 0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x11, 0x07, 0xcb, 0x96, 0x56, 0x54, 0xa4, 0xc2, 0xf0, 0x16, 0x80,
    0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x0c, 0x84, 0xa2, 0x00, 0x01, 0x00, 0x06, 0x44, 0x01, 0xc0,
    0x73, 0xc1, 0x89
};

// general_profile_idc 2, Main 10.
static constexpr Array<u8, 113> MAIN_10_CONFIGURATION_RECORD {
    0x01, 0x02, 0x20, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xfd, 0xfa,
    0xfa, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x18, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x02, 0x20,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0x95, 0x94, 0x09, 0xa1, 0x00,
    0x01, 0x00, 0x2d, 0x42, 0x01, 0x01, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x03, 0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x11, 0x07, 0xca, 0xd9, 0x65, 0x65, 0x4a, 0x4c, 0x2f, 0x01, 0x68,
    0x08, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x40, 0xa2, 0x00, 0x01, 0x00, 0x06, 0x44,
    0x01, 0xc0, 0x73, 0xc1, 0x89
};

// general_profile_idc 4, Main 12.
static constexpr Array<u8, 110> MAIN_12_CONFIGURATION_RECORD {
    0x01, 0x04, 0x08, 0x00, 0x00, 0x00, 0x99, 0x88, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xfd, 0xfc,
    0xfc, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x17, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08,
    0x00, 0x00, 0x03, 0x00, 0x99, 0x88, 0x00, 0x00, 0x03, 0x00, 0x00, 0x78, 0x95, 0x94, 0x09, 0xa1, 0x00, 0x01,
    0x00, 0x2b, 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x99, 0x88, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x78, 0xa0, 0x03, 0xc0, 0x80, 0x11, 0x07, 0xca, 0x52, 0x96, 0x56, 0x54, 0xa4, 0xc2, 0xf0, 0x16, 0x80, 0x80,
    0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x0c, 0x84, 0xa2, 0x00, 0x01, 0x00, 0x06, 0x44, 0x01, 0xc0, 0x73,
    0xc1, 0x89
};

// general_profile_idc 4, Main 4:2:2 10.
static constexpr Array<u8, 111> MAIN_4_2_2_10_CONFIGURATION_RECORD {
    0x01, 0x04, 0x08, 0x00, 0x00, 0x00, 0x9d, 0x08, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xfe, 0xfa,
    0xfa, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x17, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08,
    0x00, 0x00, 0x03, 0x00, 0x9d, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00, 0x78, 0x95, 0x94, 0x09, 0xa1, 0x00, 0x01,
    0x00, 0x2c, 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9d, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x78, 0xb0, 0x03, 0xc0, 0x80, 0x11, 0x07, 0xc4, 0xb6, 0x59, 0x59, 0x52, 0x93, 0x0b, 0xc0, 0x5a, 0x02, 0x00,
    0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x32, 0x10, 0xa2, 0x00, 0x01, 0x00, 0x06, 0x44, 0x01, 0xc0,
    0x73, 0xc1, 0x89
};

// general_profile_idc 4, Main 4:2:2 12.
static constexpr Array<u8, 110> MAIN_4_2_2_12_CONFIGURATION_RECORD {
    0x01, 0x04, 0x08, 0x00, 0x00, 0x00, 0x99, 0x08, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xfe, 0xfc,
    0xfc, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x17, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08,
    0x00, 0x00, 0x03, 0x00, 0x99, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00, 0x78, 0x95, 0x98, 0x09, 0xa1, 0x00, 0x01,
    0x00, 0x2a, 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x99, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x78, 0xb0, 0x03, 0xc0, 0x80, 0x10, 0xe4, 0x52, 0x96, 0x56, 0x69, 0x24, 0xca, 0xf0, 0x16, 0x80, 0x80, 0x00,
    0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x0c, 0x84, 0xa2, 0x00, 0x01, 0x00, 0x07, 0x44, 0x01, 0xc1, 0x72, 0xb4,
    0x62, 0x40
};

// general_profile_idc 4, Main 4:4:4.
static constexpr Array<u8, 113> MAIN_4_4_4_CONFIGURATION_RECORD {
    0x01, 0x04, 0x08, 0x00, 0x00, 0x00, 0x9e, 0x08, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xff, 0xf8,
    0xf8, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x17, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08,
    0x00, 0x00, 0x03, 0x00, 0x9e, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00, 0x78, 0x95, 0x94, 0x09, 0xa1, 0x00, 0x01,
    0x00, 0x2c, 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9e, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x78, 0x90, 0x00, 0x78, 0x10, 0x02, 0x20, 0xf8, 0x9c, 0xb2, 0xb2, 0xa5, 0x26, 0x17, 0x80, 0xb4, 0x04, 0x00,
    0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x64, 0x20, 0xa2, 0x00, 0x01, 0x00, 0x08, 0x44, 0x01, 0xc0,
    0x73, 0x18, 0x30, 0x18, 0x90
};

// general_profile_idc 4, Main 4:4:4 10.
static constexpr Array<u8, 112> MAIN_4_4_4_10_CONFIGURATION_RECORD {
    0x01, 0x04, 0x08, 0x00, 0x00, 0x00, 0x9c, 0x08, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xff, 0xfa,
    0xfa, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x17, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08,
    0x00, 0x00, 0x03, 0x00, 0x9c, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00, 0x78, 0x95, 0x98, 0x09, 0xa1, 0x00, 0x01,
    0x00, 0x2b, 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9c, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x78, 0x90, 0x00, 0x78, 0x10, 0x02, 0x1c, 0x9b, 0x2c, 0xac, 0xd2, 0x49, 0x95, 0xe0, 0x2d, 0x01, 0x00, 0x00,
    0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x19, 0x08, 0xa2, 0x00, 0x01, 0x00, 0x08, 0x44, 0x01, 0xc1, 0x72,
    0x86, 0x0c, 0x46, 0x24
};

// general_profile_idc 4, Main 4:4:4 12.
static constexpr Array<u8, 113> MAIN_4_4_4_12_CONFIGURATION_RECORD {
    0x01, 0x04, 0x08, 0x00, 0x00, 0x00, 0x98, 0x08, 0x00, 0x00, 0x00, 0x00, 0x78, 0xf0, 0x00, 0xfc, 0xff, 0xfc,
    0xfc, 0x00, 0x00, 0x0f, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x17, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08,
    0x00, 0x00, 0x03, 0x00, 0x98, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00, 0x78, 0x95, 0x98, 0x09, 0xa1, 0x00, 0x01,
    0x00, 0x2c, 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x98, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x78, 0x90, 0x00, 0x78, 0x10, 0x02, 0x1c, 0x8a, 0x52, 0xca, 0xcd, 0x24, 0x99, 0x5e, 0x02, 0xd0, 0x10, 0x00,
    0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x01, 0x90, 0x80, 0xa2, 0x00, 0x01, 0x00, 0x08, 0x44, 0x01, 0xc1,
    0x72, 0x86, 0x0c, 0x46, 0x24
};

// ITU-T H.265 (07/2024), A.3.5: the range extensions profiles state the bit depth and chroma format they are
// limited to through the constraint flags that follow the four source flags.
// ITU-T H.265 (07/2024), 7.3.3: where each flag sits among the 48 constraint indicator bits, which open with the
// four source flags.
static constexpr size_t MAX_12BIT_CONSTRAINT_FLAG_INDEX = 4;
static constexpr size_t MAX_10BIT_CONSTRAINT_FLAG_INDEX = 5;
static constexpr size_t MAX_8BIT_CONSTRAINT_FLAG_INDEX = 6;
static constexpr size_t MAX_422CHROMA_CONSTRAINT_FLAG_INDEX = 7;
static constexpr size_t MAX_420CHROMA_CONSTRAINT_FLAG_INDEX = 8;
static constexpr size_t MAX_MONOCHROME_CONSTRAINT_FLAG_INDEX = 9;

static bool constraint_flag(Array<u8, 6> const& flags, size_t index)
{
    return (flags[index / 8] & (0x80 >> (index % 8))) != 0;
}

static void set_constraint_flag(Array<u8, 6>& flags, size_t index)
{
    flags[index / 8] |= 0x80 >> (index % 8);
}

static Optional<H265::Profile> range_extensions_profile(Array<u8, 6> const& constraint_indicator_flags)
{
    auto limited_to_12_bit = constraint_flag(constraint_indicator_flags, MAX_12BIT_CONSTRAINT_FLAG_INDEX);
    auto limited_to_10_bit = constraint_flag(constraint_indicator_flags, MAX_10BIT_CONSTRAINT_FLAG_INDEX);
    auto limited_to_8_bit = constraint_flag(constraint_indicator_flags, MAX_8BIT_CONSTRAINT_FLAG_INDEX);
    auto limited_to_4_2_2 = constraint_flag(constraint_indicator_flags, MAX_422CHROMA_CONSTRAINT_FLAG_INDEX);
    auto limited_to_4_2_0 = constraint_flag(constraint_indicator_flags, MAX_420CHROMA_CONSTRAINT_FLAG_INDEX);
    auto limited_to_monochrome = constraint_flag(constraint_indicator_flags, MAX_MONOCHROME_CONSTRAINT_FLAG_INDEX);

    if (!limited_to_12_bit)
        return {};
    u8 maximum_bit_depth = 12;
    if (limited_to_8_bit)
        maximum_bit_depth = 8;
    else if (limited_to_10_bit)
        maximum_bit_depth = 10;

    // A profile allowing more than a stream states still covers it, so the intra and still picture profiles resolve
    // to the profile whose bit depth and chroma format they constrain.
    if (limited_to_4_2_0) {
        if (maximum_bit_depth <= 8)
            return H265::Profile::Main;
        if (maximum_bit_depth <= 10)
            return H265::Profile::Main10;
        return H265::Profile::Main12;
    }
    if (limited_to_4_2_2) {
        if (maximum_bit_depth <= 10)
            return H265::Profile::Main422_10;
        return H265::Profile::Main422_12;
    }
    if (limited_to_monochrome)
        return {};
    if (maximum_bit_depth <= 8)
        return H265::Profile::Main444;
    if (maximum_bit_depth <= 10)
        return H265::Profile::Main444_10;
    return H265::Profile::Main444_12;
}

Optional<H265::Profile> H265::Parameters::profile() const
{
    // ITU-T H.265 (07/2024), A.3: profile_idc identifies a profile only within its own profile space.
    if (profile_space != 0)
        return {};

    // ITU-T H.265 (07/2024), A.3.2: general_profile_compatibility_flag[j] equal to 1 specifies that the stream
    // conforms to the profile that general_profile_idc equal to j indicates, whatever profile it names. These two
    // are the most constrained profiles a record was captured for, so a claim to either settles the answer.
    static constexpr u32 MAIN_COMPATIBILITY_FLAG = 1u << 1;
    static constexpr u32 MAIN_10_COMPATIBILITY_FLAG = 1u << 2;
    if ((profile_compatibility_flags & MAIN_COMPATIBILITY_FLAG) != 0)
        return Profile::Main;
    if ((profile_compatibility_flags & MAIN_10_COMPATIBILITY_FLAG) != 0)
        return Profile::Main10;

    switch (profile_idc) {
    case 1:
        return Profile::Main;
    case 2:
        return Profile::Main10;
    case 4:
        return range_extensions_profile(constraint_indicator_flags);
    default:
        return {};
    }
}

ReadonlyBytes H265::representative_configuration_record_for_profile(Profile profile)
{
    switch (profile) {
    case Profile::Main:
        return MAIN_CONFIGURATION_RECORD;
    case Profile::Main10:
        return MAIN_10_CONFIGURATION_RECORD;
    case Profile::Main12:
        return MAIN_12_CONFIGURATION_RECORD;
    case Profile::Main422_10:
        return MAIN_4_2_2_10_CONFIGURATION_RECORD;
    case Profile::Main422_12:
        return MAIN_4_2_2_12_CONFIGURATION_RECORD;
    case Profile::Main444:
        return MAIN_4_4_4_CONFIGURATION_RECORD;
    case Profile::Main444_10:
        return MAIN_4_4_4_10_CONFIGURATION_RECORD;
    case Profile::Main444_12:
        return MAIN_4_4_4_12_CONFIGURATION_RECORD;
    }
    VERIFY_NOT_REACHED();
}

H265::Parameters H265::canonical_parameters_for_profile(Profile profile)
{
    Parameters parameters {};
    // ITU-T H.265 (07/2024), 7.4.4: "When general_profile_space is equal to 0,
    // general_profile_compatibility_flag[ general_profile_idc ] shall be equal to 1."
    switch (profile) {
    case Profile::Main:
        parameters.profile_idc = 1;
        parameters.profile_compatibility_flags = 1u << 1;
        return parameters;
    case Profile::Main10:
        parameters.profile_idc = 2;
        parameters.profile_compatibility_flags = 1u << 2;
        return parameters;
    default:
        break;
    }

    // Only the flags the profile is read back from are set, so that the result resolves to it and states nothing
    // about the source or bit rate constraints a real stream would carry.
    auto set_flag = [&](size_t index) { set_constraint_flag(parameters.constraint_indicator_flags, index); };
    parameters.profile_idc = 4;
    parameters.profile_compatibility_flags = 1u << 4;
    set_flag(MAX_12BIT_CONSTRAINT_FLAG_INDEX);
    switch (profile) {
    case Profile::Main12:
        set_flag(MAX_422CHROMA_CONSTRAINT_FLAG_INDEX);
        set_flag(MAX_420CHROMA_CONSTRAINT_FLAG_INDEX);
        return parameters;
    case Profile::Main422_10:
        set_flag(MAX_10BIT_CONSTRAINT_FLAG_INDEX);
        set_flag(MAX_422CHROMA_CONSTRAINT_FLAG_INDEX);
        return parameters;
    case Profile::Main422_12:
        set_flag(MAX_422CHROMA_CONSTRAINT_FLAG_INDEX);
        return parameters;
    case Profile::Main444:
        set_flag(MAX_10BIT_CONSTRAINT_FLAG_INDEX);
        set_flag(MAX_8BIT_CONSTRAINT_FLAG_INDEX);
        return parameters;
    case Profile::Main444_10:
        set_flag(MAX_10BIT_CONSTRAINT_FLAG_INDEX);
        return parameters;
    case Profile::Main444_12:
        return parameters;
    default:
        VERIFY_NOT_REACHED();
    }
}

}
