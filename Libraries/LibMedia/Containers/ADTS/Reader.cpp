/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/Containers/ADTS/Reader.h>
#include <LibMedia/Containers/ID3.h>

namespace Media::ADTS {

static constexpr size_t SYNC_SCAN_CHUNK_SIZE = 4096;
static constexpr size_t FRAME_COUNT_RULING_OUT_A_FALSE_SYNC_CODE = 3;

static constexpr size_t BITRATE_ESTIMATION_SCAN_BYTE_COUNT = 32 * KiB;

static size_t read_available(MediaStreamCursor& cursor, size_t position, Bytes buffer)
{
    if (cursor.seek_to_position(position).is_error())
        return 0;
    auto result = cursor.read_into(buffer);
    if (result.is_error())
        return 0;
    return result.value();
}

static bool read_exact(MediaStreamCursor& cursor, size_t position, Bytes buffer)
{
    return read_available(cursor, position, buffer) == buffer.size();
}

Optional<FrameIterator::Frame> FrameIterator::read_frame()
{
    Array<u8, FrameHeader::SIZE> frame_header_data;
    if (!read_exact(*m_cursor, m_position, frame_header_data))
        return {};

    auto header = FrameHeader::parse(frame_header_data);
    if (!header.has_value())
        return {};

    Frame frame { m_position, *header };
    m_position += header->frame_byte_size;
    return frame;
}

Optional<FrameIterator::Frame> FrameIterator::next()
{
    auto frame = read_frame();
    if (frame.has_value() || m_resynchronize == Resynchronize::No)
        return frame;

    auto boundary = Reader::find_frame_boundary_at_or_after(m_cursor, m_position + 1, NumericLimits<size_t>::max());
    if (!boundary.has_value())
        return {};

    m_position = *boundary;
    return read_frame();
}

DecoderErrorOr<FixedArray<u8>> FrameIterator::read_frame_data(Frame const& frame)
{
    auto payload_offset = frame.header.payload_offset();
    VERIFY(frame.header.frame_byte_size > payload_offset);

    TRY(m_cursor->seek_to_position(frame.position + payload_offset));
    return TRY(m_cursor->read_bytes(frame.header.frame_byte_size - payload_offset));
}

static size_t skip_identification_tags(MediaStreamCursor& cursor)
{
    size_t position = 0;
    while (true) {
        Array<u8, ID3::VERSION_2_HEADER_SIZE> tag_header;
        if (!read_exact(cursor, position, tag_header))
            break;
        auto tag_size = ID3::version_2_tag_size(tag_header);
        if (!tag_size.has_value())
            break;
        position += *tag_size;
    }
    return position;
}

struct FrameScan {
    size_t position { 0 };
    FrameHeader header;
    size_t frame_count { 0 };
    u64 sample_count { 0 };
    size_t byte_count { 0 };
};

static Optional<FrameScan> scan_frames_at(NonnullRefPtr<MediaStreamCursor> const& cursor, size_t position)
{
    FrameIterator iterator { cursor, position, FrameIterator::Resynchronize::No };

    FrameScan scan;
    while (scan.frame_count < FRAME_COUNT_RULING_OUT_A_FALSE_SYNC_CODE || scan.byte_count < BITRATE_ESTIMATION_SCAN_BYTE_COUNT) {
        auto frame = iterator.next();
        if (!frame.has_value())
            break;
        if (scan.frame_count == 0) {
            scan.position = frame->position;
            scan.header = frame->header;
        }
        scan.frame_count++;
        scan.sample_count += frame->header.sample_count;
        scan.byte_count += frame->header.frame_byte_size;
    }

    if (scan.frame_count == 0)
        return {};
    return scan;
}

// The length of a frame differs from one to the next, and so does the count of blocks within it,
// but the rest of what a header describes holds for all the frames of a stream.
static bool headers_describe_the_same_stream(FrameHeader const& first, FrameHeader const& second)
{
    return first.version == second.version
        && first.audio_object_type == second.audio_object_type
        && first.sample_rate == second.sample_rate
        && first.channel_count == second.channel_count;
}

static bool begins_a_frame(NonnullRefPtr<MediaStreamCursor> const& cursor, size_t position)
{
    return FrameIterator { cursor, position, FrameIterator::Resynchronize::No }.next().has_value();
}

static bool begins_a_frame_sequence(NonnullRefPtr<MediaStreamCursor> const& cursor, size_t position)
{
    FrameIterator iterator { cursor, position, FrameIterator::Resynchronize::No };

    Optional<FrameHeader> previous_header;
    for (size_t frame_count = 0; frame_count < FRAME_COUNT_RULING_OUT_A_FALSE_SYNC_CODE; frame_count++) {
        auto frame = iterator.next();
        if (!frame.has_value())
            return false;
        if (previous_header.has_value() && !headers_describe_the_same_stream(*previous_header, frame->header))
            return false;
        previous_header = frame->header;
    }
    return true;
}

static bool has_sync_code_at(ReadonlyBytes bytes, size_t index)
{
    if (index + 2 > bytes.size())
        return false;
    return FrameHeader::has_sync_code(static_cast<u16>((bytes[index] << 8) | bytes[index + 1]));
}

Optional<size_t> Reader::find_frame_boundary_at_or_after(NonnullRefPtr<MediaStreamCursor> const& cursor, size_t start_byte, size_t upper_bound)
{
    auto scan_position = start_byte;

    Array<u8, SYNC_SCAN_CHUNK_SIZE> buffer;
    while (scan_position + FrameHeader::SIZE <= upper_bound) {
        auto chunk = buffer.span().trim(min(SYNC_SCAN_CHUNK_SIZE, upper_bound - scan_position));
        chunk = chunk.trim(read_available(*cursor, scan_position, chunk));
        if (chunk.size() < 2)
            break;

        for (size_t index = 0; index + 1 < chunk.size(); index++) {
            if (!has_sync_code_at(chunk, index))
                continue;
            if (begins_a_frame_sequence(cursor, scan_position + index))
                return scan_position + index;
        }

        if (chunk.size() < SYNC_SCAN_CHUNK_SIZE)
            break;
        scan_position += chunk.size() - 1;
    }

    return {};
}

Optional<size_t> Reader::find_frame_boundary_at_or_before(NonnullRefPtr<MediaStreamCursor> const& cursor, size_t target_byte, size_t lower_bound, size_t upper_bound)
{
    VERIFY(target_byte <= upper_bound);

    auto chunk_end = min(target_byte + 2, upper_bound);

    Array<u8, SYNC_SCAN_CHUNK_SIZE> buffer;
    while (chunk_end >= lower_bound + 2) {
        auto chunk_start = chunk_end > SYNC_SCAN_CHUNK_SIZE ? chunk_end - SYNC_SCAN_CHUNK_SIZE : 0;
        chunk_start = max(chunk_start, lower_bound);

        auto chunk = buffer.span().trim(chunk_end - chunk_start);
        if (!read_exact(*cursor, chunk_start, chunk))
            return {};

        for (size_t index = chunk.size(); index-- > 0;) {
            if (!has_sync_code_at(chunk, index))
                continue;
            auto candidate = chunk_start + index;
            if (candidate > target_byte)
                continue;
            if (begins_a_frame_sequence(cursor, candidate))
                return candidate;
        }

        if (chunk_start == lower_bound)
            break;
        chunk_end = chunk_start + 1;
    }

    return {};
}

static Optional<size_t> find_first_frame_position(NonnullRefPtr<MediaStreamCursor> const& cursor)
{
    auto position = skip_identification_tags(*cursor);
    if (begins_a_frame(cursor, position))
        return position;
    return Reader::find_frame_boundary_at_or_after(cursor, position, position + Reader::MAXIMUM_SYNC_SEARCH_BYTE_COUNT);
}

Optional<Reader> Reader::from_stream(NonnullRefPtr<MediaStreamCursor> const& cursor)
{
    auto position = find_first_frame_position(cursor);
    if (!position.has_value())
        return {};

    auto scan = scan_frames_at(cursor, *position);
    if (!scan.has_value())
        return {};

    Reader reader;
    reader.m_frame_header = scan->header;
    reader.m_first_frame_position = scan->position;

    if (scan->byte_count == 0)
        return reader;

    auto audio_byte_count = scan->byte_count;
    if (auto stream_byte_count = cursor->size(); stream_byte_count.has_value() && *stream_byte_count > reader.m_first_frame_position)
        audio_byte_count = max(audio_byte_count, static_cast<size_t>(*stream_byte_count) - reader.m_first_frame_position);

    auto estimated_sample_count = static_cast<u64>(audio_byte_count) * scan->sample_count / scan->byte_count;
    reader.m_duration = AK::Duration::from_time_units(static_cast<i64>(estimated_sample_count), 1, scan->header.sample_rate);
    return reader;
}

}
