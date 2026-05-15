/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/MediaStream.h>

namespace Media::Codecs {

template<Integral T>
static Optional<T> read_big_endian_value(MediaStreamCursor& cursor)
{
    auto value = cursor.read_value<T>(AK::Endianness::Big);
    if (value.is_error())
        return {};
    return value.release_value();
}

static Optional<u64> read_coded_number(MediaStreamCursor& cursor)
{
    auto first_byte = read_big_endian_value<u8>(cursor);
    if (!first_byte.has_value())
        return {};
    u64 value = first_byte.value();

    auto length = count_leading_zeroes_safe(static_cast<u8>(~value));
    if (length == 0)
        return value;
    if (length == 1)
        return {};
    if (length > 7)
        return {};

    value &= (1 << (8 - (length + 1))) - 1;
    while (--length > 0) {
        auto continuation_byte = read_big_endian_value<u8>(cursor);
        if (!continuation_byte.has_value())
            return {};
        if (continuation_byte.value() >> 6 != 0b10)
            return {};
        value <<= 6;
        value |= continuation_byte.value() & 0b111111;
    }

    return value;
}

static bool verify_header_crc(MediaStreamCursor& cursor, size_t header_start)
{
    constexpr auto lookup_table = [] {
        Array<u8, 256> result;
        for (size_t i = 0; i < result.size(); i++) {
            u8 value = i;
            for (auto j = 0; j < 8; j++)
                value = (value << 1) ^ (value & 0x80 ? 0x07 : 0);
            result[i] = value;
        }
        return result;
    }();

    auto header_end = cursor.position();
    VERIFY(header_end > header_start);
    auto header_size = header_end - header_start;
    Array<u8, 16> buffer;
    if (header_size > buffer.size())
        return false;

    auto crc = read_big_endian_value<u8>(cursor);
    if (!crc.has_value())
        return false;

    if (cursor.seek_to_position(header_start).is_error())
        return false;
    auto header_data = buffer.span().trim(header_size);
    if (cursor.read_until_filled(header_data).is_error())
        return false;
    if (cursor.skip(1).is_error())
        return false;

    u8 actual_crc = 0;
    for (auto const& byte : header_data)
        actual_crc = lookup_table[actual_crc ^ byte];
    return crc.value() == actual_crc;
}

bool FLAC::is_sync_code(u16 value)
{
    return (value >> 1) == 0b111111111111100;
}

Optional<FLAC::FrameInfo> FLAC::parse_frame_header(MediaStreamCursor& cursor, u16 sync_code, u16 fixed_block_size)
{
    auto header_start = cursor.position();

    Array<u8, 4> header;
    if (cursor.read_until_filled(header).is_error())
        return {};

    u16 maybe_sync_code = (static_cast<u16>(header[0]) << 8) | header[1];
    if (maybe_sync_code != sync_code)
        return {};

    bool variable_block_size = (sync_code & 1) != 0;
    u8 block_size_bits = (header[2] >> 4) & 0x0F;
    u8 sample_rate_bits = header[2] & 0x0F;
    u8 channels_bits = (header[3] >> 4) & 0x0F;
    u8 bit_depth_bits = (header[3] >> 1) & 0x07;
    u8 reserved_bit = header[3] & 0x01;

    if (reserved_bit != 0)
        return {};

    auto coded_number = read_coded_number(cursor);
    if (!coded_number.has_value())
        return {};

    u16 block_size = 0;
    if (block_size_bits == 0b0000)
        return {};
    if (block_size_bits == 0b0001) {
        block_size = 192;
    } else if (block_size_bits <= 0b0101) {
        block_size = 144 * (1 << block_size_bits);
    } else if (block_size_bits == 0b0110) {
        auto uncommon_block_size_minus_1 = read_big_endian_value<u8>(cursor);
        if (!uncommon_block_size_minus_1.has_value())
            return {};
        block_size = uncommon_block_size_minus_1.value() + 1;
    } else if (block_size_bits == 0b0111) {
        auto uncommon_block_size_minus_1 = read_big_endian_value<u16>(cursor);
        if (!uncommon_block_size_minus_1.has_value())
            return {};
        block_size = uncommon_block_size_minus_1.value() + 1;
    } else if (block_size_bits >= 0b1000) {
        block_size = 1 << block_size_bits;
    }

    if (sample_rate_bits == 0b1100 && cursor.skip(1).is_error())
        return {};
    if ((sample_rate_bits == 0b1101 || sample_rate_bits == 0b1110) && cursor.skip(2).is_error())
        return {};
    if (sample_rate_bits == 0b1111)
        return {};

    if (channels_bits >= 0b1011)
        return {};

    if (bit_depth_bits == 0b011)
        return {};

    if (!verify_header_crc(cursor, header_start))
        return {};

    u64 sample_number = coded_number.value();
    if (!variable_block_size)
        sample_number *= fixed_block_size;

    return FrameInfo { sample_number, block_size };
}

}
