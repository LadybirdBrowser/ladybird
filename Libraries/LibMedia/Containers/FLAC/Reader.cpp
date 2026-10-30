/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Containers/FLAC/Reader.h>
#include <LibMedia/Containers/ID3.h>

namespace Media::FLAC {

static constexpr size_t SCAN_CHUNK_SIZE = 4 * KiB;
// No frame of the largest block, channel count and bit depth the format allows comes near this.
static constexpr size_t MAXIMUM_FRAME_SIZE = 4 * MiB;
// Junk between the metadata and the first frame is covered within this distance.
static constexpr size_t MAXIMUM_SYNC_SEARCH_BYTE_COUNT = 64 * KiB;
static constexpr size_t SEEK_POINT_SIZE = 18;
// A seek point a second for eighteen hours; a table claiming more is not read any further.
static constexpr size_t MAXIMUM_SEEK_POINT_COUNT = 64 * KiB;
static constexpr u64 PLACEHOLDER_SEEK_POINT_SAMPLE_NUMBER = NumericLimits<u64>::max();
static constexpr size_t ID3_VERSION_1_TAG_SIZE = 128;

namespace {

enum class MetadataBlockType : u8 {
    StreamInfo = 0,
    SeekTable = 3,
};

}

DecoderErrorOr<Optional<FrameHeader>> FrameIterator::parse_header_at(size_t position)
{
    if (m_cursor->seek_to_position(position).is_error())
        return Optional<FrameHeader> {};
    auto frame_info = TRY(Codecs::FLAC::parse_frame_header(*m_cursor, m_sync.sync_code, m_sync.fixed_block_size));
    if (!frame_info.has_value())
        return Optional<FrameHeader> {};
    return FrameHeader { position, m_cursor->position() - position, frame_info->sample_number, frame_info->block_size };
}

DecoderErrorOr<Optional<FrameHeader>> FrameIterator::find_header_at_or_after(size_t position, size_t bound)
{
    u8 sync_low_byte = m_sync.sync_code & 0xFF;
    // The chunks overlap by a byte so that a sync code split between two is seen.
    Array<u8, SCAN_CHUNK_SIZE + 1> chunk;
    auto chunk_start = position;
    while (chunk_start + 1 < bound) {
        auto chunk_size = min(chunk.size(), bound - chunk_start);
        TRY(m_cursor->seek_to_position(chunk_start));
        auto read_count = TRY(m_cursor->read_into(chunk.span().trim(chunk_size)));
        if (read_count < 2)
            return Optional<FrameHeader> {};
        for (size_t i = 0; i + 1 < read_count; i++) {
            if (chunk[i] != 0xFF || chunk[i + 1] != sync_low_byte)
                continue;
            if (auto header = TRY(parse_header_at(chunk_start + i)); header.has_value())
                return header;
        }
        chunk_start += read_count - 1;
    }
    return Optional<FrameHeader> {};
}

DecoderErrorOr<Optional<FrameHeader>> FrameIterator::find_header_before(size_t bound, size_t lower_bound)
{
    u8 sync_low_byte = m_sync.sync_code & 0xFF;
    Array<u8, SCAN_CHUNK_SIZE + 1> chunk;
    auto chunk_end = bound;
    while (chunk_end > lower_bound + 1) {
        auto chunk_start = chunk_end - lower_bound > chunk.size() ? chunk_end - chunk.size() : lower_bound;
        auto chunk_size = chunk_end - chunk_start;
        TRY(m_cursor->seek_to_position(chunk_start));
        auto read_count = TRY(m_cursor->read_into(chunk.span().trim(chunk_size)));
        if (read_count != chunk_size)
            return Optional<FrameHeader> {};
        for (size_t i = chunk_size - 1; i-- > 0;) {
            if (chunk[i] != 0xFF || chunk[i + 1] != sync_low_byte)
                continue;
            auto header = TRY(parse_header_at(chunk_start + i));
            if (header.has_value() && header->position + header->byte_size <= bound)
                return header;
        }
        if (chunk_start == lower_bound)
            break;
        chunk_end = chunk_start + 1;
    }
    return Optional<FrameHeader> {};
}

// A damaged frame fails its CRC, so the frame after it is told by its place in the sequence instead, with the frame
// after that confirming it.
DecoderErrorOr<bool> FrameIterator::frame_follows(FrameHeader const& header, FrameHeader const& candidate)
{
    if (candidate.sample_number <= header.sample_number)
        return false;
    if (candidate.sample_number == header.sample_number + header.block_size)
        return true;
    auto following = TRY(find_header_at_or_after(candidate.position + candidate.byte_size, candidate.position + MAXIMUM_FRAME_SIZE));
    return following.has_value() && following->sample_number == candidate.sample_number + candidate.block_size;
}

DecoderErrorOr<bool> FrameIterator::resync_forward(size_t bound)
{
    auto header_or_error = find_header_at_or_after(m_position, bound);
    if (header_or_error.is_error()) {
        if (header_or_error.error().category() == DecoderErrorCategory::EndOfStream)
            return false;
        return header_or_error.release_error();
    }
    if (!header_or_error.value().has_value())
        return false;
    m_position = header_or_error.value()->position;
    return true;
}

DecoderErrorOr<Optional<FrameIterator::Frame>> FrameIterator::read_frame_at(FrameHeader const& header)
{
    u8 sync_low_byte = m_sync.sync_code & 0xFF;
    auto frame_start = header.position;
    auto limit = frame_start + MAXIMUM_FRAME_SIZE;

    ByteBuffer frame_bytes;
    // The next frame's header cannot begin inside this frame's header or its two CRC bytes.
    auto scan_offset = max<size_t>(header.byte_size, 2);
    while (true) {
        auto read_position = frame_start + frame_bytes.size();
        if (read_position >= limit)
            return Optional<Frame> {};

        auto chunk_size = min<size_t>(SCAN_CHUNK_SIZE, limit - read_position);
        auto scanned_size = frame_bytes.size();
        DECODER_TRY_ALLOC(frame_bytes.try_resize(scanned_size + chunk_size));
        TRY(m_cursor->seek_to_position(read_position));
        auto read_result = m_cursor->read_into(frame_bytes.span().slice(scanned_size));
        if (read_result.is_error()) {
            if (read_result.error().category() != DecoderErrorCategory::EndOfStream || scanned_size < header.byte_size)
                return read_result.release_error();
            frame_bytes.trim(scanned_size, true);
            break;
        }
        auto read_count = read_result.value();
        frame_bytes.trim(scanned_size + read_count, true);
        if (read_count == 0)
            return Optional<Frame> {};

        for (size_t i = scan_offset; i + 1 < frame_bytes.size(); i++) {
            if (frame_bytes[i] != 0xFF || frame_bytes[i + 1] != sync_low_byte)
                continue;
            auto candidate = TRY(parse_header_at(frame_start + i));
            if (!candidate.has_value())
                continue;
            u16 frame_crc = (frame_bytes[i - 2] << 8) | frame_bytes[i - 1];
            auto frame_is_intact = Codecs::FLAC::compute_frame_crc(frame_bytes.span().trim(i - 2)) == frame_crc;
            if (!frame_is_intact && !TRY(frame_follows(header, *candidate)))
                continue;
            return Frame { header, DECODER_TRY_ALLOC(FixedArray<u8>::create(frame_bytes.span().trim(i))) };
        }
        scan_offset = frame_bytes.size() - 1;
    }

    // The last frame of a stream runs to its end, less a tag written after the audio.
    if (frame_bytes.size() >= header.byte_size + ID3_VERSION_1_TAG_SIZE) {
        auto tag = frame_bytes.span().slice(frame_bytes.size() - ID3_VERSION_1_TAG_SIZE);
        if (StringView { tag.data(), 3 } == "TAG"sv)
            frame_bytes.trim(frame_bytes.size() - ID3_VERSION_1_TAG_SIZE, true);
    }
    return Frame { header, DECODER_TRY_ALLOC(FixedArray<u8>::create(frame_bytes.span())) };
}

Optional<FrameHeader> FrameIterator::find_last_header_before(size_t bound, size_t lower_bound)
{
    auto header_or_error = find_header_before(bound, lower_bound);
    if (header_or_error.is_error())
        return {};
    return header_or_error.release_value();
}

DecoderErrorOr<FrameIterator::Frame> FrameIterator::read_next_frame()
{
    auto position = m_position;
    while (true) {
        Optional<Frame> frame;
        if (auto header = TRY(parse_header_at(position)); header.has_value())
            frame = TRY(read_frame_at(*header));
        if (frame.has_value()) {
            m_position = frame->end_position();
            return frame.release_value();
        }

        // No frame heads the position, or what heads it has no end within a frame's reach, so it was not a header.
        if (m_resynchronize == Resynchronize::No)
            return DecoderError::corrupted("No FLAC frame at the expected position"sv);
        auto next_header = TRY(find_header_at_or_after(position + 1, NumericLimits<size_t>::max()));
        if (!next_header.has_value())
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);
        position = next_header->position;
    }
}

// Finds the first frame, which follows the metadata directly unless a writer left junk between them.
static DecoderErrorOr<FrameHeader> find_first_frame(MediaStreamCursor& cursor, size_t position, Codecs::FLAC::StreamInfo const& stream_info, FrameSync& frame_sync)
{
    auto header_at = [&](size_t candidate) -> DecoderErrorOr<Optional<FrameHeader>> {
        if (cursor.seek_to_position(candidate).is_error())
            return Optional<FrameHeader> {};
        auto sync_code_or_error = cursor.read_value<u16>();
        if (sync_code_or_error.is_error() || !Codecs::FLAC::is_sync_code(sync_code_or_error.value()))
            return Optional<FrameHeader> {};
        auto sync_code = sync_code_or_error.value();
        u16 fixed_block_size = (sync_code & 1) != 0 ? 0 : stream_info.maximum_block_size;
        if (cursor.seek_to_position(candidate).is_error())
            return Optional<FrameHeader> {};
        auto frame_info = TRY(Codecs::FLAC::parse_frame_header(cursor, sync_code, fixed_block_size));
        if (!frame_info.has_value())
            return Optional<FrameHeader> {};
        frame_sync = FrameSync { sync_code, fixed_block_size };
        return FrameHeader { candidate, cursor.position() - candidate, frame_info->sample_number, frame_info->block_size };
    };

    if (auto header = TRY(header_at(position)); header.has_value())
        return header.release_value();

    Array<u8, SCAN_CHUNK_SIZE + 1> chunk;
    auto chunk_start = position;
    auto bound = position + MAXIMUM_SYNC_SEARCH_BYTE_COUNT;
    while (chunk_start + 1 < bound) {
        TRY(cursor.seek_to_position(chunk_start));
        auto read_count = TRY(cursor.read_into(chunk.span().trim(min(chunk.size(), bound - chunk_start))));
        if (read_count < 2)
            break;
        for (size_t i = 0; i + 1 < read_count; i++) {
            if (chunk[i] != 0xFF || (chunk[i + 1] & 0xFE) != 0xF8)
                continue;
            if (auto header = TRY(header_at(chunk_start + i)); header.has_value())
                return header.release_value();
        }
        chunk_start += read_count - 1;
    }
    return DecoderError::corrupted("No FLAC frame follows the metadata"sv);
}

DecoderErrorOr<Reader> Reader::from_stream(NonnullRefPtr<MediaStreamCursor> const& cursor)
{
    size_t position = 0;
    Array<u8, ID3::VERSION_2_HEADER_SIZE> tag_header;
    TRY(cursor->seek_to_position(0));
    auto tag_header_read = cursor->read_into(tag_header.span());
    if (tag_header_read.is_error() || tag_header_read.value() != tag_header.size())
        return DecoderError::with_description(DecoderErrorCategory::UnrecognizedFormat, "Stream is too short to be FLAC"sv);
    if (auto tag_size = ID3::version_2_tag_size(tag_header); tag_size.has_value())
        position = *tag_size;

    Array<u8, 4> marker;
    TRY(cursor->seek_to_position(position));
    auto marker_read = cursor->read_into(marker.span());
    if (marker_read.is_error() || marker_read.value() != marker.size() || StringView { marker.data(), marker.size() } != "fLaC"sv)
        return DecoderError::with_description(DecoderErrorCategory::UnrecognizedFormat, "Stream does not begin with the FLAC marker"sv);
    position += marker.size();

    ByteBuffer codec_initialization_data;
    DECODER_TRY_ALLOC(codec_initialization_data.try_append("fLaC"sv.bytes()));
    Optional<Codecs::FLAC::StreamInfo> stream_info;
    Vector<SeekPoint> seek_points;
    while (true) {
        TRY(cursor->seek_to_position(position));
        auto block_header = TRY(cursor->read_value<u32>());
        auto is_last_block = (block_header >> 31) != 0;
        auto block_type = static_cast<MetadataBlockType>((block_header >> 24) & 0x7F);
        size_t block_size = block_header & 0xFFFFFF;
        auto block_body = position + sizeof(u32);

        if (!stream_info.has_value()) {
            if (block_type != MetadataBlockType::StreamInfo || block_size != Codecs::FLAC::STREAM_INFO_SIZE)
                return DecoderError::corrupted("FLAC stream does not begin with STREAMINFO"sv);
            Array<u8, Codecs::FLAC::STREAM_INFO_SIZE> block;
            TRY(cursor->read_until_filled(block.span()));
            stream_info = TRY(Codecs::FLAC::parse_stream_info(block));
            // The decoder's copy stands alone, so it is the last block.
            DECODER_TRY_ALLOC(codec_initialization_data.try_append(Array<u8, 4> { 0x80, 0x00, 0x00, Codecs::FLAC::STREAM_INFO_SIZE }.span()));
            DECODER_TRY_ALLOC(codec_initialization_data.try_append(block.span()));
        } else if (block_type == MetadataBlockType::SeekTable && seek_points.is_empty()) {
            if (block_size % SEEK_POINT_SIZE != 0)
                return DecoderError::corrupted("FLAC SEEKTABLE has a partial seek point"sv);
            auto seek_point_count = min(block_size / SEEK_POINT_SIZE, MAXIMUM_SEEK_POINT_COUNT);
            for (size_t i = 0; i < seek_point_count; i++) {
                auto sample_number = TRY(cursor->read_value<u64>());
                auto byte_offset = TRY(cursor->read_value<u64>());
                TRY(cursor->read_value<u16>()); // The samples in the frame, which the frame itself tells.
                if (sample_number != PLACEHOLDER_SEEK_POINT_SAMPLE_NUMBER)
                    DECODER_TRY_ALLOC(seek_points.try_append({ sample_number, byte_offset }));
            }
        }

        position = block_body + block_size;
        if (is_last_block)
            break;
    }

    FrameSync frame_sync { 0xFFF8, stream_info->maximum_block_size };
    auto first_frame_position = position;
    auto first_frame = find_first_frame(*cursor, position, *stream_info, frame_sync);
    if (!first_frame.is_error())
        first_frame_position = first_frame.value().position;
    else if (first_frame.error().category() != DecoderErrorCategory::EndOfStream)
        return first_frame.release_error();
    return Reader { *stream_info, frame_sync, first_frame_position, move(seek_points), move(codec_initialization_data) };
}

Reader::Reader(Codecs::FLAC::StreamInfo stream_info, FrameSync frame_sync, size_t first_frame_position, Vector<SeekPoint> seek_points, ByteBuffer codec_initialization_data)
    : m_stream_info(stream_info)
    , m_frame_sync(frame_sync)
    , m_first_frame_position(first_frame_position)
    , m_seek_points(move(seek_points))
    , m_codec_initialization_data(move(codec_initialization_data))
{
}

AK::Duration Reader::duration() const
{
    return AK::Duration::from_time_units(AK::clamp_to<i64>(m_stream_info.total_sample_count), 1, m_stream_info.sample_rate);
}

}
