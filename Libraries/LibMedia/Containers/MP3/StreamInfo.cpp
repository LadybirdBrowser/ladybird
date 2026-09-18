/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibMedia/BitReader.h>
#include <LibMedia/Containers/MP3/FrameHeader.h>
#include <LibMedia/Containers/MP3/StreamInfo.h>

namespace Media::MP3 {

static constexpr size_t TAG_SIZE = 4;

static Array<u8, TAG_SIZE> read_tag(BitReader& reader)
{
    Array<u8, TAG_SIZE> tag;
    for (auto& byte : tag)
        byte = reader.read_bits<u8>(8);
    return tag;
}

static size_t side_information_size(FrameHeader const& header)
{
    if (header.version == MPEGVersion::Version1)
        return header.channel_count == 1 ? 17 : 32;
    return header.channel_count == 1 ? 9 : 17;
}

static size_t tag_offset(FrameHeader const& header)
{
    auto offset = FrameHeader::SIZE + side_information_size(header);
    if (header.has_crc)
        offset += FrameHeader::CRC_SIZE;
    return offset;
}

static void read_encoder_delays(BitReader& reader, StreamInfo& stream_info)
{
    static constexpr size_t ENCODER_NAME_SIZE = 9;
    static constexpr size_t BYTE_COUNT_BEFORE_DELAYS = 12;
    static constexpr size_t DELAY_BIT_COUNT = 12;

    Array<u8, ENCODER_NAME_SIZE> encoder_name;
    for (auto& byte : encoder_name)
        byte = reader.read_bits<u8>(8);

    auto encoder = StringView { encoder_name }.substring_view(0, TAG_SIZE);
    if (encoder != "LAME"sv && encoder != "Lavf"sv && encoder != "Lavc"sv)
        return;

    reader.skip_bits(BYTE_COUNT_BEFORE_DELAYS * 8);
    auto delays = reader.read_bits<u32>(DELAY_BIT_COUNT * 2);
    if (reader.has_overrun())
        return;

    stream_info.encoder_delay = static_cast<u16>(delays >> DELAY_BIT_COUNT);
    stream_info.encoder_padding = static_cast<u16>(delays & ((1 << DELAY_BIT_COUNT) - 1));
}

static Optional<StreamInfo> parse_xing_tag(ReadonlyBytes frame, FrameHeader const& header)
{
    static constexpr u32 FRAME_COUNT_FLAG = 0x01;
    static constexpr u32 BYTE_COUNT_FLAG = 0x02;
    static constexpr u32 SEEK_TABLE_FLAG = 0x04;
    static constexpr u32 QUALITY_FLAG = 0x08;

    BitReader reader { frame };
    reader.skip_bits(tag_offset(header) * 8);

    auto tag = read_tag(reader);
    auto tag_view = StringView { tag };
    if (tag_view != "Xing"sv && tag_view != "Info"sv)
        return {};

    auto flags = reader.read_bits<u32>(32);

    StreamInfo stream_info;
    if ((flags & FRAME_COUNT_FLAG) != 0)
        stream_info.frame_count = reader.read_bits<u32>(32);
    if ((flags & BYTE_COUNT_FLAG) != 0)
        stream_info.byte_count = reader.read_bits<u32>(32);
    if ((flags & SEEK_TABLE_FLAG) != 0) {
        Array<u8, StreamInfo::SEEK_TABLE_ENTRY_COUNT> seek_table;
        for (auto& entry : seek_table)
            entry = reader.read_bits<u8>(8);
        stream_info.seek_table = seek_table;
    }
    if ((flags & QUALITY_FLAG) != 0)
        reader.skip_bits(32);

    if (reader.has_overrun())
        return {};

    read_encoder_delays(reader, stream_info);
    return stream_info;
}

static Optional<StreamInfo> parse_vbri_tag(ReadonlyBytes frame)
{
    static constexpr size_t TAG_OFFSET = 36;
    static constexpr u16 SUPPORTED_VERSION = 1;

    BitReader reader { frame };
    reader.skip_bits(TAG_OFFSET * 8);

    auto tag = read_tag(reader);
    if (StringView { tag } != "VBRI"sv)
        return {};
    if (reader.read_bits<u16>(16) != SUPPORTED_VERSION)
        return {};

    reader.skip_bits(32);

    StreamInfo stream_info;
    stream_info.byte_count = reader.read_bits<u32>(32);
    stream_info.frame_count = reader.read_bits<u32>(32);
    if (reader.has_overrun())
        return {};

    return stream_info;
}

Optional<StreamInfo> StreamInfo::parse(ReadonlyBytes frame, FrameHeader const& header)
{
    static constexpr u8 LAYER_III = 3;
    if (header.layer != LAYER_III)
        return {};

    if (auto stream_info = parse_xing_tag(frame, header); stream_info.has_value())
        return stream_info;
    return parse_vbri_tag(frame);
}

Optional<u64> StreamInfo::total_sample_count(FrameHeader const& header) const
{
    if (!frame_count.has_value())
        return {};

    return static_cast<u64>(*frame_count) * header.sample_count;
}

}
