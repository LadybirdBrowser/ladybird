/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibMedia/Containers/WAV/Reader.h>

namespace Media::WAV {

static constexpr size_t RIFF_HEADER_SIZE = 12;
static constexpr size_t CHUNK_HEADER_SIZE = 8;
static constexpr u32 FORMAT_CHUNK_SIZE = 16;
static constexpr u32 EXTENSIBLE_FORMAT_CHUNK_SIZE = 40;

static constexpr u16 FORMAT_TAG_PCM = 0x0001;
static constexpr u16 FORMAT_TAG_IEEE_FLOAT = 0x0003;
static constexpr u16 FORMAT_TAG_ALAW = 0x0006;
static constexpr u16 FORMAT_TAG_MULAW = 0x0007;
static constexpr u16 FORMAT_TAG_EXTENSIBLE = 0xFFFE;

// The subformat GUIDs of the extensible header begin with a format tag and share this tail.
static constexpr Array<u8, 14> MEDIA_SUBTYPE_GUID_TAIL { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

using ChunkType = Array<u8, 4>;

static bool chunk_type_is(ReadonlyBytes type, StringView name)
{
    return StringView { type.data(), type.size() } == name;
}

static DecoderErrorOr<CodecID> codec_id_for(u16 format_tag, u16 bits_per_sample)
{
    if (format_tag == FORMAT_TAG_PCM) {
        if (bits_per_sample == 8)
            return CodecID::U8;
        if (bits_per_sample == 16)
            return CodecID::S16LE;
        if (bits_per_sample == 24)
            return CodecID::S24LE;
        if (bits_per_sample == 32)
            return CodecID::S32LE;
    } else if (format_tag == FORMAT_TAG_IEEE_FLOAT) {
        if (bits_per_sample == 32)
            return CodecID::F32LE;
    } else if (format_tag == FORMAT_TAG_ALAW) {
        if (bits_per_sample == 8)
            return CodecID::ALaw;
    } else if (format_tag == FORMAT_TAG_MULAW) {
        if (bits_per_sample == 8)
            return CodecID::MuLaw;
    }
    return DecoderError::format(DecoderErrorCategory::NotImplemented, "WAVE format {:#x} with {} bits per sample is not supported", format_tag, bits_per_sample);
}

// The speaker position bits of the extensible header follow the order of Audio::Channel after Unknown.
static Audio::ChannelMap channel_map_for(u16 channel_count, u32 speaker_position_mask)
{
    Vector<Audio::Channel, Audio::ChannelMap::capacity()> channels;
    for (u8 position = 1; position < to_underlying(Audio::Channel::Count); position++) {
        if ((speaker_position_mask >> (position - 1)) & 1)
            channels.append(static_cast<Audio::Channel>(position));
    }
    if (channels.size() == channel_count)
        return Audio::ChannelMap(channels);

    switch (channel_count) {
    case 1:
        return Audio::ChannelMap::mono();
    case 2:
        return Audio::ChannelMap::stereo();
    case 4:
        return Audio::ChannelMap::quadrophonic();
    case 6:
        return Audio::ChannelMap::surround_5_1();
    case 8:
        return Audio::ChannelMap::surround_7_1();
    default:
        break;
    }
    channels.clear();
    channels.resize(channel_count);
    return Audio::ChannelMap(channels);
}

static DecoderErrorOr<SampleFormat> parse_format_chunk(MediaStreamCursor& cursor, u32 chunk_size)
{
    if (chunk_size < FORMAT_CHUNK_SIZE)
        return DecoderError::corrupted("WAVE fmt chunk is too small"sv);

    auto format_tag = TRY(cursor.read_value<u16>(AK::Endianness::Little));
    auto channel_count = TRY(cursor.read_value<u16>(AK::Endianness::Little));
    auto sample_rate = TRY(cursor.read_value<u32>(AK::Endianness::Little));
    TRY(cursor.read_value<u32>(AK::Endianness::Little)); // Average bytes per second, which the fields around it determine.
    auto bytes_per_frame = TRY(cursor.read_value<u16>(AK::Endianness::Little));
    auto bits_per_sample = TRY(cursor.read_value<u16>(AK::Endianness::Little));

    u32 speaker_position_mask = 0;
    if (format_tag == FORMAT_TAG_EXTENSIBLE) {
        if (chunk_size < EXTENSIBLE_FORMAT_CHUNK_SIZE)
            return DecoderError::corrupted("WAVE extensible fmt chunk is too small"sv);
        TRY(cursor.read_value<u16>(AK::Endianness::Little)); // Extension size.
        TRY(cursor.read_value<u16>(AK::Endianness::Little)); // Valid bits per sample, within the container size above.
        speaker_position_mask = TRY(cursor.read_value<u32>(AK::Endianness::Little));
        format_tag = TRY(cursor.read_value<u16>(AK::Endianness::Little));
        Array<u8, MEDIA_SUBTYPE_GUID_TAIL.size()> subformat_guid_tail;
        TRY(cursor.read_until_filled(subformat_guid_tail.span()));
        if (subformat_guid_tail.span() != MEDIA_SUBTYPE_GUID_TAIL.span())
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "WAVE subformat is not a media subtype"sv);
    }

    if (channel_count == 0 || sample_rate == 0)
        return DecoderError::corrupted("WAVE fmt chunk declares no channels or no sample rate"sv);
    if (channel_count > Audio::ChannelMap::capacity())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "WAVE stream has {} channels", channel_count);

    auto codec_id = TRY(codec_id_for(format_tag, bits_per_sample));

    auto bytes_per_frame_of_samples = channel_count * ((bits_per_sample + 7) / 8);
    if (bytes_per_frame == 0)
        bytes_per_frame = bytes_per_frame_of_samples;
    else if (bytes_per_frame != bytes_per_frame_of_samples)
        return DecoderError::corrupted("WAVE block alignment disagrees with its channels and sample size"sv);

    Checked<u32> bytes_per_second = sample_rate;
    bytes_per_second *= bytes_per_frame;
    if (bytes_per_second.has_overflow())
        return DecoderError::corrupted("WAVE stream's byte rate does not fit in 32 bits"sv);

    return SampleFormat { codec_id, sample_rate, bytes_per_frame, bytes_per_second.value(), channel_map_for(channel_count, speaker_position_mask) };
}

DecoderErrorOr<Reader> Reader::from_stream(NonnullRefPtr<MediaStreamCursor> const& cursor)
{
    Array<u8, RIFF_HEADER_SIZE> riff_header;
    TRY(cursor->seek_to_position(0));
    auto riff_header_read = cursor->read_into(riff_header.span());
    if (riff_header_read.is_error() || riff_header_read.value() != riff_header.size()
        || !chunk_type_is(riff_header.span().trim(4), "RIFF"sv) || !chunk_type_is(riff_header.span().slice(8), "WAVE"sv)) {
        return DecoderError::with_description(DecoderErrorCategory::UnrecognizedFormat, "Stream does not begin with a RIFF WAVE header"sv);
    }

    auto stream_size = cursor->size();
    Optional<SampleFormat> sample_format;
    u64 chunk_position = RIFF_HEADER_SIZE;
    while (true) {
        if (stream_size.has_value() && chunk_position + CHUNK_HEADER_SIZE > *stream_size)
            return DecoderError::corrupted("WAVE header has no data chunk"sv);
        TRY(cursor->seek_to_position(chunk_position));
        ChunkType chunk_type;
        TRY(cursor->read_until_filled(chunk_type.span()));
        auto chunk_size = TRY(cursor->read_value<u32>(AK::Endianness::Little));
        auto chunk_body = chunk_position + CHUNK_HEADER_SIZE;

        if (chunk_type_is(chunk_type.span(), "fmt "sv)) {
            sample_format = TRY(parse_format_chunk(*cursor, chunk_size));
        } else if (chunk_type_is(chunk_type.span(), "data"sv)) {
            if (!sample_format.has_value())
                return DecoderError::corrupted("WAVE data chunk precedes its fmt chunk"sv);
            // Writers that stream leave the size unset or at its maximum, and no size is trusted past the stream's end.
            Optional<u64> sample_data_end;
            if (chunk_size != 0 && chunk_size != NumericLimits<u32>::max())
                sample_data_end = chunk_body + chunk_size;
            if (stream_size.has_value() && (!sample_data_end.has_value() || *sample_data_end > *stream_size))
                sample_data_end = *stream_size;
            return Reader { sample_format.release_value(), static_cast<size_t>(chunk_body), sample_data_end };
        }

        // Chunks are padded to even sizes.
        chunk_position = chunk_body + chunk_size + (chunk_size & 1);
    }
}

Reader::Reader(SampleFormat sample_format, size_t sample_data_offset, Optional<u64> sample_data_end)
    : m_sample_format(sample_format)
    , m_sample_data_offset(sample_data_offset)
    , m_sample_data_end(sample_data_end)
{
}

AK::Duration Reader::duration() const
{
    if (!m_sample_data_end.has_value() || *m_sample_data_end <= m_sample_data_offset)
        return AK::Duration::zero();
    auto frame_count = (*m_sample_data_end - m_sample_data_offset) / m_sample_format.bytes_per_frame;
    return AK::Duration::from_time_units(AK::clamp_to<i64>(frame_count), 1, m_sample_format.sample_rate);
}

}
