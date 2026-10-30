/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <AK/GenericShorthands.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/MediaStream.h>
#include <LibMediaCodecs/BitReader.h>

namespace Media::Codecs {

DecoderErrorOr<FixedArray<u8>> FLAC::codec_initialization_data_from_isobmff_configuration(ReadonlyBytes configuration)
{
    if (configuration.size() < sizeof(u32))
        return DecoderError::corrupted("FLAC configuration is too small"sv);

    auto version = configuration[0];
    if (version == 0) {
        auto flags = (configuration[1] << 16) | (configuration[2] << 8) | configuration[3];
        if (flags != 0)
            return DecoderError::corrupted("FLAC configuration has unsupported flags"sv);
    } else {
        return DecoderError::corrupted("FLAC configuration has an unsupported version"sv);
    }

    auto initialization_data = DECODER_TRY_ALLOC(FixedArray<u8>::create(configuration));
    "fLaC"sv.bytes().copy_to(initialization_data.span());
    return initialization_data;
}

static DecoderErrorOr<bool> propagate_abort(DecoderErrorOr<void> result)
{
    if (!result.is_error())
        return true;
    if (first_is_one_of(result.error().category(), DecoderErrorCategory::Aborted, DecoderErrorCategory::PermanentlyUnavailable))
        return result.release_error();
    return false;
}

template<Integral T>
static DecoderErrorOr<Optional<T>> read_big_endian_value(MediaStreamCursor& cursor)
{
    T value = 0;
    if (!TRY(propagate_abort(cursor.read_until_filled({ &value, sizeof(value) }))))
        return Optional<T> {};
    return AK::convert_between_host_and_big_endian(value);
}

static DecoderErrorOr<Optional<u64>> read_coded_number(MediaStreamCursor& cursor)
{
    auto first_byte = TRY(read_big_endian_value<u8>(cursor));
    if (!first_byte.has_value())
        return Optional<u64> {};
    u64 value = first_byte.value();

    auto length = count_leading_zeroes_safe(static_cast<u8>(~value));
    if (length == 0)
        return value;
    if (length == 1)
        return Optional<u64> {};
    if (length > 7)
        return Optional<u64> {};

    value &= (1 << (8 - (length + 1))) - 1;
    while (--length > 0) {
        auto continuation_byte = TRY(read_big_endian_value<u8>(cursor));
        if (!continuation_byte.has_value())
            return Optional<u64> {};
        if (continuation_byte.value() >> 6 != 0b10)
            return Optional<u64> {};
        value <<= 6;
        value |= continuation_byte.value() & 0b111111;
    }

    return value;
}

static DecoderErrorOr<bool> verify_header_crc(MediaStreamCursor& cursor, size_t header_start)
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

    auto crc = TRY(read_big_endian_value<u8>(cursor));
    if (!crc.has_value())
        return false;

    if (!TRY(propagate_abort(cursor.seek_to_position(header_start))))
        return false;
    auto header_data = buffer.span().trim(header_size);
    if (!TRY(propagate_abort(cursor.read_until_filled(header_data))))
        return false;
    if (!TRY(propagate_abort(cursor.skip(1))))
        return false;

    u8 actual_crc = 0;
    for (auto const& byte : header_data)
        actual_crc = lookup_table[actual_crc ^ byte];
    return crc.value() == actual_crc;
}

u16 FLAC::compute_frame_crc(ReadonlyBytes bytes)
{
    constexpr auto lookup_table = [] {
        Array<u16, 256> result;
        for (size_t i = 0; i < result.size(); i++) {
            u16 value = static_cast<u16>(i << 8);
            for (auto j = 0; j < 8; j++)
                value = static_cast<u16>((value << 1) ^ ((value & 0x8000) ? 0x8005 : 0));
            result[i] = value;
        }
        return result;
    }();

    u16 crc = 0;
    for (auto const& byte : bytes)
        crc = static_cast<u16>((crc << 8) ^ lookup_table[((crc >> 8) ^ byte) & 0xFF]);
    return crc;
}

DecoderErrorOr<FLAC::StreamInfo> FLAC::parse_stream_info(ReadonlyBytes bytes)
{
    BitReader reader { bytes };
    StreamInfo stream_info;
    stream_info.minimum_block_size = reader.read_bits<u16>(16);
    stream_info.maximum_block_size = reader.read_bits<u16>(16);
    stream_info.minimum_frame_size = reader.read_bits<u32>(24);
    stream_info.maximum_frame_size = reader.read_bits<u32>(24);
    stream_info.sample_rate = reader.read_bits<u32>(20);
    stream_info.channel_count = reader.read_bits<u8>(3) + 1;
    stream_info.bits_per_sample = reader.read_bits<u8>(5) + 1;
    stream_info.total_sample_count = reader.read_bits<u64>(36);
    if (stream_info.sample_rate == 0 || stream_info.maximum_block_size == 0)
        return DecoderError::corrupted("FLAC STREAMINFO declares no sample rate or block size"sv);
    return stream_info;
}

bool FLAC::is_sync_code(u16 value)
{
    return (value >> 1) == 0b111111111111100;
}

DecoderErrorOr<Optional<FLAC::FrameInfo>> FLAC::parse_frame_header(MediaStreamCursor& cursor, u16 sync_code, u16 fixed_block_size)
{
    auto header_start = cursor.position();

    Array<u8, 4> header;
    if (!TRY(propagate_abort(cursor.read_until_filled(header))))
        return Optional<FrameInfo> {};

    BitReader reader { header };
    if (reader.read_bits<u16>(16) != sync_code)
        return Optional<FrameInfo> {};

    bool variable_block_size = (sync_code & 1) != 0;
    auto block_size_bits = reader.read_bits<u8>(4);
    auto sample_rate_bits = reader.read_bits<u8>(4);
    auto channels_bits = reader.read_bits<u8>(4);
    auto bit_depth_bits = reader.read_bits<u8>(3);
    auto reserved_bit_is_set = reader.read_bit();

    if (reserved_bit_is_set)
        return Optional<FrameInfo> {};

    auto coded_number = TRY(read_coded_number(cursor));
    if (!coded_number.has_value())
        return Optional<FrameInfo> {};

    u16 block_size = 0;
    if (block_size_bits == 0b0000)
        return Optional<FrameInfo> {};
    if (block_size_bits == 0b0001) {
        block_size = 192;
    } else if (block_size_bits <= 0b0101) {
        block_size = 144 * (1 << block_size_bits);
    } else if (block_size_bits == 0b0110) {
        auto uncommon_block_size_minus_1 = TRY(read_big_endian_value<u8>(cursor));
        if (!uncommon_block_size_minus_1.has_value())
            return Optional<FrameInfo> {};
        block_size = uncommon_block_size_minus_1.value() + 1;
    } else if (block_size_bits == 0b0111) {
        auto uncommon_block_size_minus_1 = TRY(read_big_endian_value<u16>(cursor));
        if (!uncommon_block_size_minus_1.has_value())
            return Optional<FrameInfo> {};
        block_size = uncommon_block_size_minus_1.value() + 1;
    } else if (block_size_bits >= 0b1000) {
        block_size = 1 << block_size_bits;
    }

    if (sample_rate_bits == 0b1100 && !TRY(propagate_abort(cursor.skip(1))))
        return Optional<FrameInfo> {};
    if ((sample_rate_bits == 0b1101 || sample_rate_bits == 0b1110) && !TRY(propagate_abort(cursor.skip(2))))
        return Optional<FrameInfo> {};
    if (sample_rate_bits == 0b1111)
        return Optional<FrameInfo> {};

    if (channels_bits >= 0b1011)
        return Optional<FrameInfo> {};

    if (bit_depth_bits == 0b011)
        return Optional<FrameInfo> {};

    if (!TRY(verify_header_crc(cursor, header_start)))
        return Optional<FrameInfo> {};

    u64 sample_number = coded_number.value();
    if (!variable_block_size)
        sample_number *= fixed_block_size;

    return FrameInfo { sample_number, block_size };
}

Audio::ChannelMap FLAC::channel_map(u8 channel_count)
{
    using Audio::Channel;
    switch (channel_count) {
    case 1:
        return Audio::ChannelMap::mono();
    case 2:
        return Audio::ChannelMap::stereo();
    case 3:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontRight, Channel::FrontCenter);
    case 4:
        return Audio::ChannelMap::quadrophonic();
    case 5:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontRight, Channel::FrontCenter, Channel::BackLeft, Channel::BackRight);
    case 6:
        return Audio::ChannelMap::surround_5_1();
    case 7:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontRight, Channel::FrontCenter, Channel::LowFrequency, Channel::BackCenter, Channel::SideLeft, Channel::SideRight);
    default:
        return Audio::ChannelMap::surround_7_1();
    }
}

DecoderErrorOr<FLAC::PacketDurationParser> FLAC::PacketDurationParser::create(ReadonlyBytes codec_initialization_data)
{
    // The stream marker, then the header of the STREAMINFO block that must come first.
    static constexpr size_t STREAM_INFO_POSITION = 8;
    auto begins_with_stream_info = codec_initialization_data.size() >= STREAM_INFO_POSITION + STREAM_INFO_SIZE
        && codec_initialization_data.starts_with("fLaC"sv.bytes())
        && (codec_initialization_data[4] & 0x7F) == 0
        && codec_initialization_data.slice(5, 3) == Array<u8, 3> { 0, 0, STREAM_INFO_SIZE }.span();
    if (!begins_with_stream_info)
        return DecoderError::corrupted("FLAC codec initialization data does not begin with STREAMINFO"sv);

    auto stream_info = TRY(parse_stream_info(codec_initialization_data.slice(STREAM_INFO_POSITION, STREAM_INFO_SIZE)));
    return PacketDurationParser { stream_info.sample_rate };
}

DecoderErrorOr<u32> FLAC::PacketDurationParser::parse_duration_in_samples(MediaStreamCursor& cursor, size_t packet_size)
{
    if (packet_size < sizeof(u16))
        return DecoderError::corrupted("FLAC frame is too small"sv);
    auto sync_code = TRY(cursor.read_value<u16>());
    if (!is_sync_code(sync_code))
        return DecoderError::corrupted("FLAC frame does not begin with a sync code"sv);
    TRY(cursor.seek(-static_cast<i64>(sizeof(u16)), AK::SeekMode::FromCurrentPosition));
    auto frame = TRY(parse_frame_header(cursor, sync_code, 0));
    if (!frame.has_value())
        return DecoderError::corrupted("FLAC frame header is invalid"sv);
    return frame.value().block_size;
}

}
