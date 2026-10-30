/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/Checked.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Containers/Ogg/Reader.h>

namespace Media::Ogg {

static constexpr StringView CAPTURE_PATTERN = "OggS"sv;
static constexpr size_t SCAN_CHUNK_SIZE = 4 * KiB;
// A packet may span any number of pages, so a stream that never ends one must not grow it without bound. No frame of
// the codecs we read comes near this.
static constexpr size_t MAXIMUM_PACKET_SIZE = 32 * MiB;

template<typename T>
static T load_little_endian(ReadonlyBytes bytes, size_t offset)
{
    T value = 0;
    for (size_t i = 0; i < sizeof(T); i++)
        value |= static_cast<T>(bytes[offset + i]) << (8 * i);
    return value;
}

template<typename T>
static T load_big_endian(ReadonlyBytes bytes, size_t offset)
{
    T value = 0;
    for (size_t i = 0; i < sizeof(T); i++)
        value = static_cast<T>(value << 8) | bytes[offset + i];
    return value;
}

static bool verify_page_crc(ReadonlyBytes header, ReadonlyBytes segment_sizes, ReadonlyBytes payload)
{
    constexpr size_t checksum_offset = 22;
    constexpr auto lookup_table = [] {
        Array<u32, 256> result;
        for (u32 i = 0; i < result.size(); i++) {
            u32 value = i << 24;
            for (auto bit = 0; bit < 8; bit++)
                value = (value << 1) ^ ((value & 0x80000000) != 0 ? 0x04C11DB7 : 0);
            result[i] = value;
        }
        return result;
    }();

    u32 crc = 0;
    auto add = [&](u8 byte) { crc = (crc << 8) ^ lookup_table[((crc >> 24) & 0xFF) ^ byte]; };
    for (size_t i = 0; i < header.size(); i++)
        add(i >= checksum_offset && i < checksum_offset + 4 ? 0 : header[i]);
    for (auto byte : segment_sizes)
        add(byte);
    for (auto byte : payload)
        add(byte);

    auto expected_crc = static_cast<u32>(header[checksum_offset])
        | (static_cast<u32>(header[checksum_offset + 1]) << 8)
        | (static_cast<u32>(header[checksum_offset + 2]) << 16)
        | (static_cast<u32>(header[checksum_offset + 3]) << 24);
    return crc == expected_crc;
}

static DecoderErrorOr<bool> try_read_exactly_at(MediaStreamCursor& cursor, size_t position, Bytes bytes)
{
    if (bytes.is_empty())
        return true;
    TRY(cursor.seek_to_position(position));
    auto read_count = cursor.read_into(bytes);
    if (read_count.is_error()) {
        if (read_count.error().category() == DecoderErrorCategory::EndOfStream)
            return false;
        return read_count.release_error();
    }
    return read_count.value() == bytes.size();
}

DecoderErrorOr<PageHeaderReadResult> PageIterator::read_header_at(size_t position, FixedArray<u8>* segment_sizes)
{
    Array<u8, PAGE_HEADER_SIZE> header;
    if (!TRY(try_read_exactly_at(*m_cursor, position, header)))
        return PageReadFailure::Incomplete;
    if (!header.span().starts_with(CAPTURE_PATTERN.bytes()) || header[4] != 0)
        return PageReadFailure::Invalid;

    auto segment_count = header[26];
    Array<u8, 255> segment_table;
    auto segments = segment_table.span().trim(segment_count);
    if (!TRY(try_read_exactly_at(*m_cursor, position + PAGE_HEADER_SIZE, segments)))
        return PageReadFailure::Incomplete;

    size_t payload_size = 0;
    for (auto size : segments)
        payload_size += size;

    if (segment_sizes != nullptr)
        *segment_sizes = DECODER_TRY_ALLOC(FixedArray<u8>::create(segments));

    return PageHeader {
        .position = position,
        .byte_size = PAGE_HEADER_SIZE + segment_count + payload_size,
        .flags = header[5],
        .granule_position = bit_cast<i64>(load_little_endian<u64>(header, 6)),
        .serial_number = load_little_endian<u32>(header, 14),
        .sequence_number = load_little_endian<u32>(header, 18),
    };
}

DecoderErrorOr<Optional<Page>> PageIterator::read_page_at(size_t position)
{
    FixedArray<u8> segment_sizes;
    auto result = TRY(read_header_at(position, &segment_sizes));
    auto const* header = result.get_pointer<PageHeader>();
    if (header == nullptr)
        return Optional<Page> {};

    Array<u8, PAGE_HEADER_SIZE> header_bytes;
    if (!TRY(try_read_exactly_at(*m_cursor, position, header_bytes)))
        return Optional<Page> {};
    auto payload_position = position + PAGE_HEADER_SIZE + segment_sizes.size();
    auto payload = DECODER_TRY_ALLOC(FixedArray<u8>::create(header->end_position() - payload_position));
    if (!TRY(try_read_exactly_at(*m_cursor, payload_position, payload.span())))
        return Optional<Page> {};
    if (!verify_page_crc(header_bytes, segment_sizes.span(), payload.span()))
        return Optional<Page> {};

    return Page { *header, move(segment_sizes), move(payload) };
}

DecoderErrorOr<bool> PageIterator::resync_forward(size_t bound)
{
    constexpr size_t capture_pattern_overlap = CAPTURE_PATTERN.length() - 1;
    Array<u8, SCAN_CHUNK_SIZE + capture_pattern_overlap> chunk;
    auto chunk_start = m_position;
    while (chunk_start < bound) {
        auto chunk_size = chunk.size();
        if (bound - chunk_start < chunk_size - capture_pattern_overlap)
            chunk_size = bound - chunk_start + capture_pattern_overlap;
        TRY(m_cursor->seek_to_position(chunk_start));
        auto read_count = m_cursor->read_into(chunk.span().trim(chunk_size));
        if (read_count.is_error()) {
            if (read_count.error().category() == DecoderErrorCategory::EndOfStream)
                return false;
            return read_count.release_error();
        }
        if (read_count.value() < CAPTURE_PATTERN.length())
            return false;
        auto bytes = chunk.span().trim(read_count.value());
        for (size_t i = 0; i + CAPTURE_PATTERN.length() <= bytes.size() && chunk_start + i < bound; i++) {
            if (!bytes.slice(i).starts_with(CAPTURE_PATTERN.bytes()))
                continue;
            if (!TRY(read_page_at(chunk_start + i)).has_value())
                continue;
            m_position = chunk_start + i;
            return true;
        }
        chunk_start += bytes.size() - capture_pattern_overlap;
    }
    return false;
}

DecoderErrorOr<Optional<PageHeader>> PageIterator::find_last_page_before(size_t bound, size_t lower_bound)
{
    constexpr size_t capture_pattern_overlap = CAPTURE_PATTERN.length() - 1;
    Array<u8, SCAN_CHUNK_SIZE + capture_pattern_overlap> chunk;
    auto chunk_end = bound;
    while (chunk_end > lower_bound) {
        auto chunk_start = max(lower_bound, chunk_end > SCAN_CHUNK_SIZE ? chunk_end - SCAN_CHUNK_SIZE : 0);
        auto chunk_size = min(chunk_end + capture_pattern_overlap, bound) - chunk_start;
        TRY(m_cursor->seek_to_position(chunk_start));
        auto read_count = m_cursor->read_into(chunk.span().trim(chunk_size));
        if (read_count.is_error()) {
            if (read_count.error().category() != DecoderErrorCategory::EndOfStream)
                return read_count.release_error();
            read_count = 0;
        }
        auto bytes = chunk.span().trim(read_count.value());
        for (size_t i = bytes.size(); i-- > 0;) {
            if (chunk_start + i >= chunk_end || !bytes.slice(i).starts_with(CAPTURE_PATTERN.bytes()))
                continue;
            auto page = TRY(read_page_at(chunk_start + i));
            if (page.has_value() && page->header.end_position() <= bound)
                return page->header;
        }
        chunk_end = chunk_start;
    }
    return Optional<PageHeader> {};
}

DecoderErrorOr<Page> PageIterator::read_next_page()
{
    while (true) {
        if (auto page = TRY(read_page_at(m_position)); page.has_value()) {
            m_position = page->header.end_position();
            return page.release_value();
        }
        auto resync_iterator = *this;
        resync_iterator.m_position++;
        if (!TRY(resync_iterator.resync_forward()))
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "No page follows"sv);
        m_position = resync_iterator.m_position;
    }
}

DecoderErrorOr<PageHeaderReadResult> PageIterator::read_next_page_header()
{
    auto result = TRY(read_header_at(m_position, nullptr));
    if (auto const* header = result.get_pointer<PageHeader>())
        m_position = header->end_position();
    return result;
}

Optional<PacketFragment> PacketFragmentIterator::next()
{
    if (m_segment_sizes.is_empty())
        return {};
    size_t byte_count = 0;
    size_t segment_count = 0;
    bool ends_packet = false;
    for (auto segment_size : m_segment_sizes) {
        byte_count += segment_size;
        segment_count++;
        if (segment_size < MAXIMUM_SEGMENT_SIZE) {
            ends_packet = true;
            break;
        }
    }
    PacketFragment fragment { m_payload.trim(byte_count), m_starts_packet, ends_packet };
    m_segment_sizes = m_segment_sizes.slice(segment_count);
    m_payload = m_payload.slice(byte_count);
    m_starts_packet = true;
    return fragment;
}

Optional<ReadonlyBytes> Page::first_complete_packet() const
{
    auto fragment = packet_fragments().next();
    if (fragment.has_value() && fragment->starts_packet && fragment->ends_packet)
        return fragment->bytes;
    return {};
}

void PacketAssembler::reset()
{
    m_partial_packet = {};
    m_expected_sequence_number = {};
}

DecoderErrorOr<Vector<FixedArray<u8>>> PacketAssembler::take_completed_packets(Page const& page)
{
    if (m_expected_sequence_number != page.header.sequence_number || !page.header.continues_packet())
        m_partial_packet = {};
    m_expected_sequence_number = page.header.sequence_number + 1;

    Vector<FixedArray<u8>> packets;
    auto fragments = page.packet_fragments();
    for (auto fragment = fragments.next(); fragment.has_value(); fragment = fragments.next()) {
        if (fragment->starts_packet)
            m_partial_packet = ByteBuffer {};
        if (!m_partial_packet.has_value())
            continue;
        if (m_partial_packet->size() + fragment->bytes.size() > MAXIMUM_PACKET_SIZE) {
            reset();
            return DecoderError::corrupted("Packet is too large"sv);
        }
        DECODER_TRY_ALLOC(m_partial_packet->try_append(fragment->bytes));
        if (fragment->ends_packet) {
            DECODER_TRY_ALLOC(packets.try_append(DECODER_TRY_ALLOC(FixedArray<u8>::create(m_partial_packet->bytes()))));
            m_partial_packet = {};
        }
    }
    return packets;
}

static Audio::ChannelMap vorbis_channel_map(u8 channel_count)
{
    using Audio::Channel;
    switch (channel_count) {
    case 1:
        return Audio::ChannelMap::mono();
    case 2:
        return Audio::ChannelMap::stereo();
    case 3:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontCenter, Channel::FrontRight);
    case 4:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontRight, Channel::BackLeft, Channel::BackRight);
    case 5:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontCenter, Channel::FrontRight, Channel::BackLeft, Channel::BackRight);
    case 6:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontCenter, Channel::FrontRight, Channel::BackLeft, Channel::BackRight, Channel::LowFrequency);
    case 7:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontCenter, Channel::FrontRight, Channel::SideLeft, Channel::SideRight, Channel::BackCenter, Channel::LowFrequency);
    case 8:
        return Audio::ChannelMap(Channel::FrontLeft, Channel::FrontCenter, Channel::FrontRight, Channel::SideLeft, Channel::SideRight, Channel::BackLeft, Channel::BackRight, Channel::LowFrequency);
    default:
        return Audio::ChannelMap::invalid();
    }
}

Track StreamIdentification::create_track(size_t identifier) const
{
    Track track { type, identifier, Track::Kind::Main, {}, {} };
    track.set_parsed_codec(ParsedCodec { codec_id });
    if (type == TrackType::Video) {
        track.set_video_data({ .pixel_width = pixel_width, .pixel_height = pixel_height, .cicp = {} });
        return track;
    }
    auto channel_map = vorbis_channel_map(channel_count);
    if (codec_id == CodecID::FLAC)
        channel_map = Codecs::FLAC::channel_map(channel_count);
    track.set_audio_data({ .sample_specification = Audio::SampleSpecification(sample_rate, channel_map) });
    return track;
}

i64 StreamIdentification::end_units_of_granule_position(i64 granule_position) const
{
    VERIFY(granule_position >= 0);
    if (codec_id == CodecID::Theora) {
        auto keyframe_part = granule_position >> theora_keyframe_granule_shift;
        auto frames_since_keyframe = granule_position & ((static_cast<i64>(1) << theora_keyframe_granule_shift) - 1);
        auto frame_count = keyframe_part + frames_since_keyframe;
        if (!theora_granule_counts_frames_from_one)
            frame_count++;
        return frame_count;
    }
    if (codec_id == CodecID::Opus)
        return granule_position - opus_pre_skip;
    return granule_position;
}

i64 StreamIdentification::theora_keyframe_start_units(i64 granule_position) const
{
    VERIFY(codec_id == CodecID::Theora && granule_position >= 0);
    auto keyframe_part = granule_position >> theora_keyframe_granule_shift;
    if (theora_granule_counts_frames_from_one)
        return max<i64>(keyframe_part - 1, 0);
    return keyframe_part;
}

AK::Duration StreamIdentification::time_of_units(i64 units) const
{
    if (type == TrackType::Video)
        return AK::Duration::from_time_units(units, frame_rate_denominator, frame_rate_numerator);
    return AK::Duration::from_time_units(units, 1, sample_rate);
}

i64 StreamIdentification::units_of_time(AK::Duration time) const
{
    if (type == TrackType::Video)
        return time.to_time_units(frame_rate_denominator, frame_rate_numerator);
    return time.to_time_units(1, sample_rate);
}

static DecoderErrorOr<StreamIdentification> identify_vorbis(ReadonlyBytes packet)
{
    if (packet.size() < 30 || load_little_endian<u32>(packet, 7) != 0)
        return DecoderError::corrupted("Vorbis identification header is invalid"sv);
    StreamIdentification identification;
    identification.codec_id = CodecID::Vorbis;
    identification.header_packet_count = 3;
    identification.channel_count = packet[11];
    identification.sample_rate = load_little_endian<u32>(packet, 12);
    if (identification.channel_count == 0 || identification.sample_rate == 0)
        return DecoderError::corrupted("Vorbis identification header is invalid"sv);
    return identification;
}

static DecoderErrorOr<StreamIdentification> identify_opus(ReadonlyBytes packet)
{
    constexpr u8 major_version_mask = 0xF0;
    if (packet.size() < 19 || (packet[8] & major_version_mask) != 0)
        return DecoderError::corrupted("Opus identification header is invalid"sv);
    StreamIdentification identification;
    identification.codec_id = CodecID::Opus;
    identification.header_packet_count = 2;
    identification.channel_count = packet[9];
    identification.opus_pre_skip = load_little_endian<u16>(packet, 10);
    identification.sample_rate = 48'000;
    if (identification.channel_count == 0)
        return DecoderError::corrupted("Opus identification header is invalid"sv);
    return identification;
}

static constexpr size_t OGG_FLAC_STREAM_INFO_OFFSET = 17;

static DecoderErrorOr<StreamIdentification> identify_flac(ReadonlyBytes packet)
{
    // The mapping header, the native stream marker, then the STREAMINFO block with its own header.
    if (packet.size() < OGG_FLAC_STREAM_INFO_OFFSET + Codecs::FLAC::STREAM_INFO_SIZE || packet[5] != 1
        || !packet.slice(9).starts_with("fLaC"sv.bytes()) || (packet[13] & 0x7F) != 0)
        return DecoderError::corrupted("FLAC identification header is invalid"sv);
    auto stream_info = packet.slice(OGG_FLAC_STREAM_INFO_OFFSET, Codecs::FLAC::STREAM_INFO_SIZE);
    StreamIdentification identification;
    identification.codec_id = CodecID::FLAC;
    auto following_header_count = load_big_endian<u16>(packet, 7);
    if (following_header_count != 0)
        identification.header_packet_count = following_header_count + 1;
    identification.sample_rate = (static_cast<u32>(stream_info[10]) << 12) | (static_cast<u32>(stream_info[11]) << 4) | (stream_info[12] >> 4);
    identification.channel_count = ((stream_info[12] >> 1) & 0x07) + 1;
    if (identification.sample_rate == 0)
        return DecoderError::corrupted("FLAC identification header is invalid"sv);
    return identification;
}

static DecoderErrorOr<StreamIdentification> identify_theora(ReadonlyBytes packet)
{
    if (packet.size() < 42 || packet[7] != 3 || packet[8] != 2)
        return DecoderError::corrupted("Theora identification header is invalid"sv);
    auto load24 = [&](size_t offset) {
        return (static_cast<u32>(packet[offset]) << 16) | (static_cast<u32>(packet[offset + 1]) << 8) | packet[offset + 2];
    };
    StreamIdentification identification;
    identification.codec_id = CodecID::Theora;
    identification.type = TrackType::Video;
    identification.header_packet_count = 3;
    identification.pixel_width = load24(14);
    identification.pixel_height = load24(17);
    identification.frame_rate_numerator = load_big_endian<u32>(packet, 22);
    identification.frame_rate_denominator = load_big_endian<u32>(packet, 26);
    identification.theora_keyframe_granule_shift = ((packet[40] & 0x03) << 3) | (packet[41] >> 5);
    identification.theora_granule_counts_frames_from_one = packet[9] >= 1;
    if (identification.pixel_width == 0 || identification.pixel_height == 0 || identification.frame_rate_numerator == 0 || identification.frame_rate_denominator == 0)
        return DecoderError::corrupted("Theora identification header is invalid"sv);
    return identification;
}

DecoderErrorOr<Optional<StreamIdentification>> identify_stream(ReadonlyBytes first_packet)
{
    if (first_packet.starts_with("\x01vorbis"sv.bytes()))
        return TRY(identify_vorbis(first_packet));
    if (first_packet.starts_with("OpusHead"sv.bytes()))
        return TRY(identify_opus(first_packet));
    if (first_packet.starts_with("\177FLAC"sv.bytes()))
        return TRY(identify_flac(first_packet));
    if (first_packet.starts_with("\x80theora"sv.bytes()))
        return TRY(identify_theora(first_packet));
    return Optional<StreamIdentification> {};
}

bool is_header_packet(StreamIdentification const& identification, ReadonlyBytes packet)
{
    switch (identification.codec_id) {
    case CodecID::Vorbis:
        return !packet.is_empty() && (packet[0] & 0x01) != 0;
    case CodecID::Theora:
        return !packet.is_empty() && (packet[0] & 0x80) != 0;
    case CodecID::Opus:
        return packet.starts_with("OpusHead"sv.bytes()) || packet.starts_with("OpusTags"sv.bytes());
    case CodecID::FLAC:
        // Metadata blocks begin with their type, and only frames begin with the first byte of their sync code.
        return packet.is_empty() || packet[0] != 0xFF;
    default:
        VERIFY_NOT_REACHED();
    }
}

bool is_theora_keyframe(ReadonlyBytes packet)
{
    return !packet.is_empty() && (packet[0] & 0xC0) == 0;
}

DecoderErrorOr<FixedArray<u8>> build_codec_initialization_data(StreamIdentification const& identification, Vector<FixedArray<u8>> const& header_packets)
{
    VERIFY(!header_packets.is_empty());
    switch (identification.codec_id) {
    case CodecID::Opus:
        return DECODER_TRY_ALLOC(FixedArray<u8>::create(header_packets[0].span()));
    case CodecID::FLAC: {
        // The native stream marker and a STREAMINFO block marked as the last, which is what FLAC streams start with.
        auto stream_info = header_packets[0].span().slice(OGG_FLAC_STREAM_INFO_OFFSET, Codecs::FLAC::STREAM_INFO_SIZE);
        auto data = DECODER_TRY_ALLOC(FixedArray<u8>::create(8 + Codecs::FLAC::STREAM_INFO_SIZE));
        "fLaC"sv.bytes().copy_to(data.span());
        data[4] = 0x80;
        data[5] = 0;
        data[6] = 0;
        data[7] = Codecs::FLAC::STREAM_INFO_SIZE;
        stream_info.copy_to(data.span().slice(8));
        return data;
    }
    case CodecID::Vorbis:
    case CodecID::Theora: {
        // The three headers in Xiph lacing: a count of all but the last, then all but the last one's sizes.
        if (header_packets.size() != 3)
            return DecoderError::corrupted("Stream does not have three header packets"sv);
        Checked<size_t> size = 1;
        for (size_t i = 0; i < 2; i++)
            size += (header_packets[i].size() / 255) + 1;
        for (auto const& packet : header_packets)
            size += packet.size();
        if (size.has_overflow())
            return DecoderError::corrupted("Header packets are too large"sv);
        auto data = DECODER_TRY_ALLOC(FixedArray<u8>::create(size.value()));
        size_t offset = 0;
        data[offset++] = 2;
        for (size_t i = 0; i < 2; i++) {
            auto remaining = header_packets[i].size();
            while (remaining >= 255) {
                data[offset++] = 255;
                remaining -= 255;
            }
            data[offset++] = static_cast<u8>(remaining);
        }
        for (auto const& packet : header_packets) {
            packet.span().copy_to(data.span().slice(offset));
            offset += packet.size();
        }
        return data;
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

}
