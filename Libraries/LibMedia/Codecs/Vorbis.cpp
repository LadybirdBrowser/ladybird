/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <AK/Vector.h>
#include <LibMedia/Codecs/Vorbis.h>
#include <LibMedia/MediaStream.h>
#include <LibMediaCodecs/BitReader.h>

namespace Media::Codecs {

static constexpr size_t IDENTIFICATION_HEADER_SIZE = 30;
static constexpr size_t COMMON_HEADER_SIZE = 7;
static constexpr u8 MAXIMUM_MODE_COUNT = 64;
// The mode number and the previous window flag must fit in the first byte of a packet, after the packet type bit.
static constexpr u8 MAXIMUM_SUPPORTED_MODE_COUNT = 63;

static bool is_header_of_type(ReadonlyBytes header, u8 type)
{
    return header.size() >= COMMON_HEADER_SIZE && header[0] == type && header.slice(1, 6) == "vorbis"sv.bytes();
}

static DecoderErrorOr<Array<ReadonlyBytes, 3>> split_headers(ReadonlyBytes codec_initialization_data)
{
    // The three headers in Xiph lacing: a count of all but the last, then all but the last one's sizes.
    if (codec_initialization_data.is_empty() || codec_initialization_data[0] != 2)
        return DecoderError::corrupted("Vorbis codec initialization data does not contain three headers"sv);

    size_t offset = 1;
    Array<size_t, 2> laced_sizes {};
    for (auto& size : laced_sizes) {
        while (true) {
            if (offset >= codec_initialization_data.size())
                return DecoderError::corrupted("Vorbis header sizes are truncated"sv);
            auto lace = codec_initialization_data[offset++];
            size += lace;
            if (lace < 255)
                break;
        }
    }

    auto headers = codec_initialization_data.slice(offset);
    if (laced_sizes[0] + laced_sizes[1] > headers.size())
        return DecoderError::corrupted("Vorbis headers are truncated"sv);
    return Array {
        headers.trim(laced_sizes[0]),
        headers.slice(laced_sizes[0], laced_sizes[1]),
        headers.slice(laced_sizes[0] + laced_sizes[1]),
    };
}

DecoderErrorOr<Vorbis::PacketDurationParser> Vorbis::PacketDurationParser::create(ReadonlyBytes codec_initialization_data)
{
    auto headers = TRY(split_headers(codec_initialization_data));

    // https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-630004.2.2
    auto identification_header = headers[0];
    if (identification_header.size() < IDENTIFICATION_HEADER_SIZE || !is_header_of_type(identification_header, 1))
        return DecoderError::corrupted("Vorbis identification header is invalid"sv);

    auto read_u32 = [&](size_t offset) {
        u32 value = 0;
        for (size_t i = 0; i < sizeof(u32); i++)
            value |= static_cast<u32>(identification_header[offset + i]) << (8 * i);
        return value;
    };
    auto version = read_u32(7);
    PacketDurationParser parser;
    parser.m_samples_per_second = read_u32(12);
    auto block_size_exponents = identification_header[28];
    auto short_block_size_exponent = block_size_exponents & 0xF;
    auto long_block_size_exponent = block_size_exponents >> 4;
    auto header_is_valid = [&] {
        if (version != 0 || parser.m_samples_per_second == 0)
            return false;
        return short_block_size_exponent >= 6 && long_block_size_exponent <= 13 && short_block_size_exponent <= long_block_size_exponent;
    }();
    if (!header_is_valid)
        return DecoderError::corrupted("Vorbis identification header is invalid"sv);
    parser.m_block_sizes = { static_cast<u16>(1u << short_block_size_exponent), static_cast<u16>(1u << long_block_size_exponent) };

    // https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-650004.2.4
    // The modes end the setup header, but finding them from the front means decoding every codebook, floor, residue
    // and mapping before them. Instead, as FFmpeg and liboggz do, walk back from the framing bit through every run of
    // fields that could be a mode, and take the longest one preceded by a matching mode count.
    auto setup_header = headers[2];
    if (!is_header_of_type(setup_header, 5))
        return DecoderError::corrupted("Vorbis setup header is invalid"sv);

    // Fields are packed from the least significant bit, so reading the reversed bytes from their most significant
    // bit reads each field backwards from its end.
    Vector<u8> reversed_setup_header;
    reversed_setup_header.resize(setup_header.size());
    for (size_t i = 0; i < setup_header.size(); i++)
        reversed_setup_header[i] = setup_header[setup_header.size() - 1 - i];

    // The mode fields, plus a mode count and the setup header's packet type and signature before them.
    static constexpr size_t MINIMUM_BITS_BEFORE_A_MODE = 97;

    BitReader reader { reversed_setup_header };
    Optional<size_t> bit_count_through_framing_bit;
    while (reader.bits_remaining() > MINIMUM_BITS_BEFORE_A_MODE) {
        if (reader.read_bit()) {
            bit_count_through_framing_bit = reader.bit_position();
            break;
        }
    }
    if (!bit_count_through_framing_bit.has_value())
        return DecoderError::corrupted("Vorbis setup header has no framing bit"sv);

    u8 candidate_mode_count = 0;
    while (reader.bits_remaining() >= MINIMUM_BITS_BEFORE_A_MODE) {
        // Backwards, a mode is its mapping, transform type, window type and block flag. Both types must be zero, and
        // there are at most 64 mappings.
        auto mapping = reader.read_bits<u8>(8);
        auto transform_type = reader.read_bits<u16>(16);
        auto window_type = reader.read_bits<u16>(16);
        if (mapping >= MAXIMUM_MODE_COUNT || transform_type != 0 || window_type != 0)
            break;
        reader.skip_bits(1);
        candidate_mode_count++;
        if (candidate_mode_count > MAXIMUM_MODE_COUNT)
            break;
        auto mode_count_reader = reader;
        if (mode_count_reader.read_bits<u8>(6) + 1u == candidate_mode_count)
            parser.m_mode_count = candidate_mode_count;
    }
    if (parser.m_mode_count == 0)
        return DecoderError::corrupted("Vorbis setup header has no modes"sv);
    if (parser.m_mode_count > MAXIMUM_SUPPORTED_MODE_COUNT)
        return DecoderError::not_implemented();

    reader = BitReader { reversed_setup_header };
    reader.skip_bits(bit_count_through_framing_bit.value());
    for (u8 mode = parser.m_mode_count; mode-- > 0;) {
        reader.skip_bits(40);
        if (reader.read_bit())
            parser.m_long_block_modes |= 1ull << mode;
    }

    // https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-730004.3.1
    // ilog(mode_count - 1), where ilog(0) is 0.
    if (parser.m_mode_count > 1)
        parser.m_mode_bit_count = count_required_bits(static_cast<u8>(parser.m_mode_count - 1));
    parser.reset();
    return parser;
}

DecoderErrorOr<u32> Vorbis::PacketDurationParser::parse_duration_in_samples(MediaStreamCursor& cursor, size_t packet_size)
{
    if (packet_size == 0)
        return 0;

    // https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-730004.3.1
    auto first_byte = TRY(cursor.read_value<u8>());
    if ((first_byte & 1) != 0)
        return DecoderError::corrupted("Vorbis packet is not an audio packet"sv);
    auto mode = (first_byte >> 1) & ((1u << m_mode_bit_count) - 1);
    if (mode >= m_mode_count)
        return DecoderError::corrupted("Vorbis packet has an invalid mode"sv);

    auto is_long_block = ((m_long_block_modes >> mode) & 1) != 0;
    auto block_size = m_block_sizes[is_long_block ? 1 : 0];
    auto previous_block_size = m_previous_block_size;
    if (is_long_block) {
        auto previous_window_flag = (first_byte >> (1 + m_mode_bit_count)) & 1;
        previous_block_size = m_block_sizes[previous_window_flag];
    }
    m_previous_block_size = block_size;

    // https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-800004.3.8
    // The returned data runs from the center of the previous window to the center of the current one.
    return (previous_block_size / 4u) + (block_size / 4u);
}

void Vorbis::PacketDurationParser::reset()
{
    m_previous_block_size = m_block_sizes[0];
}

}
