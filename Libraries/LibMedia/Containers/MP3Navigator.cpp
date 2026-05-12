/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MP3Navigator.h"

#include <AK/BinarySearch.h>
#include <AK/Endian.h>
#include <AK/IntegralMath.h>
#include <LibSync/Mutex.h>

namespace Media {

static bool read_exact(MediaStreamCursor& cursor, Bytes buffer)
{
    auto result = cursor.read_into(buffer);
    return !result.is_error() && result.value() == buffer.size();
}

template<Integral T, Integral V>
static bool read(MediaStreamCursor& cursor, V& value)
{
    T read_value = 0;
    bool result = read_exact(cursor, { &read_value, sizeof(read_value) });
    value = AK::convert_between_host_and_big_endian(read_value);
    return result;
}

template<Integral T>
static bool seek(MediaStreamCursor& cursor, T position)
{
    if (position > NumericLimits<i64>::max())
        return false;
    auto result = cursor.seek(static_cast<i64>(position), SeekMode::SetPosition);
    return !result.is_error();
}

static constexpr i16 BITRATES[2][3][16] = {
    // Version 1
    {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1 },     // Layer III
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, -1 },    // Layer II
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, -1 }, // Layer I
    },
    // Version 2/2.5
    {
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1 },      // Layer III
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1 },      // Layer II
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, -1 }, // Layer I
    }
};

static constexpr u16 SAMPLES_PER_FRAME[2][3] = {
    // Version 1
    {
        1152, // Layer III
        1152, // Layer II
        384,  // Layer I
    },
    // Version 2/2.5
    {
        576,  // Layer III
        1152, // Layer II
        384,  // Layer I
    },
};

static constexpr u16 SAMPLING_RATES[4][4] = {
    { 11025, 12000, 8000, 0 },  // Version 2.5
    { 0, 0, 0, 0 },             // Reserved
    { 22050, 24000, 16000, 0 }, // Version 2
    { 44100, 48000, 32000, 0 }, // Version 1
};

template<Unsigned T>
static bool has_sync_code(T value)
{
    constexpr auto all_bits = static_cast<T>(-1);
    constexpr auto shift = NumericLimits<T>::digits() - 11;
    constexpr auto sync_code = static_cast<T>(all_bits << shift);
    return (value & sync_code) == sync_code;
}

static bool has_sync_code(ReadonlyBytes bytes, size_t start)
{
    if (bytes.size() < start + 2)
        return false;
    auto value = static_cast<u16>((bytes[start] << 8) | bytes[start + 1]);
    return has_sync_code(value);
}

template<Unsigned T>
static u8 read_field(T value, u8 offset, u8 size)
{
    constexpr auto bit_count = NumericLimits<T>::digits();
    auto mask = (static_cast<T>(1) << size) - 1;
    return (value >> (bit_count - offset - size)) & mask;
}

static constexpr u64 EXACT_MPEG_AUDIO_DURATION_TIMEBASE = 14'112'000;

struct FrameInfo {
    u16 frame_byte_size { 0 };
    u32 duration_in_ticks { 0 };
};

static bool parse_frame_header(MediaStreamCursor& cursor, FrameInfo& frame_info)
{
    u32 frame_header_data = 0;
    if (!read<u32>(cursor, frame_header_data))
        return false;

    if (!has_sync_code(frame_header_data))
        return false;

    u8 mpeg_version = read_field(frame_header_data, 11, 2);
    if (mpeg_version == 0b01)
        return false;
    auto is_mpeg_version_2 = mpeg_version != 0b11;

    u8 layer_description = read_field(frame_header_data, 13, 2);
    if (layer_description == 0b00)
        return false;
    auto is_layer_i = layer_description == 0b11;

    auto layer_description_index = layer_description - 1;
    u16 frame_samples = SAMPLES_PER_FRAME[is_mpeg_version_2][layer_description_index];

    u8 bitrate_description = read_field(frame_header_data, 16, 4);
    i16 bitrate = BITRATES[is_mpeg_version_2][layer_description_index][bitrate_description];
    if (bitrate <= 0)
        return false;

    u8 sampling_frequency_index = read_field(frame_header_data, 20, 2);
    u16 sampling_frequency = SAMPLING_RATES[mpeg_version][sampling_frequency_index];
    if (sampling_frequency == 0)
        return false;

    u8 padding_bit = read_field(frame_header_data, 22, 1);

    constexpr size_t bytes_per_kb = 1000 / 8;
    size_t slot_size = is_layer_i ? 4 : 1;
    auto slot_count = static_cast<u64>(frame_samples) * static_cast<u64>(bitrate) * bytes_per_kb / (sampling_frequency * slot_size);
    frame_info.frame_byte_size = static_cast<u16>((slot_count + padding_bit) * slot_size);

    frame_info.duration_in_ticks = static_cast<u64>(frame_samples) * EXACT_MPEG_AUDIO_DURATION_TIMEBASE / sampling_frequency;
    return true;
}

static AK::Duration duration_from_ticks(u64 ticks)
{
    return AK::Duration::from_time_units(static_cast<i64>(ticks), 1, EXACT_MPEG_AUDIO_DURATION_TIMEBASE);
}

static AK::Duration scale_duration_by_byte_ratio(AK::Duration duration, size_t numerator, size_t denominator)
{
    VERIFY(numerator <= denominator);
    VERIFY(denominator > 0);

    auto denominator_bit_width = AK::log2(denominator) + 1;
    if (denominator_bit_width > NumericLimits<u32>::digits()) {
        auto shift = denominator_bit_width - NumericLimits<u32>::digits();
        numerator >>= shift;
        denominator >>= shift;
    }

    return duration.scaled_by(static_cast<u32>(numerator), static_cast<u32>(denominator));
}

struct ByteTimePoint {
    size_t byte;
    AK::Duration time;
};

struct EnclosingCachedRanges {
    MP3Navigator::CachedRange before;
    Optional<MP3Navigator::CachedRange> after;
};

struct FrameScanResult {
    ByteTimePoint scanned_end;
    ByteTimePoint last_parsed_frame;
    Optional<SeekedPosition> frame_spanning_target;
};

static int compare_byte_range_end_to_position(MediaStream::ByteRange const& range, size_t position)
{
    return range.end <= position ? -1 : 1;
}

static AK::Duration interpolate_timestamp_at_byte(size_t byte, ByteTimePoint const& left, ByteTimePoint const& right)
{
    if (byte <= left.byte)
        return left.time;
    if (byte >= right.byte)
        return right.time;
    if (right.time <= left.time)
        return left.time;

    return left.time + scale_duration_by_byte_ratio(right.time - left.time, byte - left.byte, right.byte - left.byte);
}

static size_t estimate_byte_for_timestamp(AK::Duration timestamp, ByteTimePoint const& left, ByteTimePoint const& right)
{
    auto estimated_byte = left.byte;
    if (right.time > left.time && right.byte > left.byte) {
        auto numerator = (timestamp - left.time).to_time_units(1, EXACT_MPEG_AUDIO_DURATION_TIMEBASE);
        auto denominator = (right.time - left.time).to_time_units(1, EXACT_MPEG_AUDIO_DURATION_TIMEBASE);
        if (denominator > 0) {
            auto byte_span = static_cast<i64>(right.byte - left.byte);
            if (Checked<i64>::multiplication_would_overflow(numerator, byte_span)) {
                estimated_byte = right.byte;
            } else {
                estimated_byte = left.byte + static_cast<size_t>(numerator * byte_span / denominator);
            }
        }
    }
    return estimated_byte;
}

static constexpr int FRAME_VALIDATION_COUNT = 3;

static bool has_valid_frame_sequence_at(MediaStreamCursor& cursor, size_t candidate, size_t upper_bound)
{
    auto position = candidate;
    for (int i = 0; i < FRAME_VALIDATION_COUNT; i++) {
        if (position + 4 > upper_bound)
            return false;
        if (!seek(cursor, position))
            return false;
        FrameInfo frame_info;
        if (!parse_frame_header(cursor, frame_info))
            return false;
        position += frame_info.frame_byte_size;
    }
    return true;
}

static constexpr size_t RESYNC_CHUNK_SIZE = 4096;

static Optional<size_t> find_frame_boundary_at_or_before(MediaStreamCursor& cursor, size_t target_byte, size_t lower_bound, size_t upper_bound)
{
    VERIFY(target_byte > lower_bound);
    VERIFY(target_byte <= upper_bound);

    auto chunk_end = min(target_byte + 2, upper_bound);
    auto chunk_start = chunk_end > RESYNC_CHUNK_SIZE ? chunk_end - RESYNC_CHUNK_SIZE : 0;
    chunk_start = max(chunk_start, lower_bound);
    auto chunk_size = chunk_end - chunk_start;
    if (chunk_size < 2)
        return {};

    Array<u8, RESYNC_CHUNK_SIZE> buffer;
    if (!seek(cursor, chunk_start))
        return {};
    if (!read_exact(cursor, buffer.span().trim(chunk_size)))
        return {};

    auto bytes = buffer.span().trim(chunk_size);
    for (size_t i = bytes.size(); i-- > 0;) {
        if (!has_sync_code(bytes, i))
            continue;
        auto candidate = chunk_start + i;
        if (candidate > target_byte)
            continue;
        if (has_valid_frame_sequence_at(cursor, candidate, upper_bound))
            return candidate;
    }
    return {};
}

static Optional<size_t> find_frame_boundary_at_or_after(MediaStreamCursor& cursor, size_t start_byte, size_t upper_bound)
{
    VERIFY(start_byte < upper_bound);
    auto chunk_start = start_byte;
    auto chunk_size = min(RESYNC_CHUNK_SIZE, upper_bound - chunk_start);
    if (chunk_size < 2)
        return {};

    Array<u8, RESYNC_CHUNK_SIZE> buffer;
    if (!seek(cursor, chunk_start))
        return {};
    if (!read_exact(cursor, buffer.span().trim(chunk_size)))
        return {};

    auto bytes = buffer.span().trim(chunk_size);
    for (size_t i = 0; i < bytes.size(); i++) {
        if (!has_sync_code(bytes, i))
            continue;
        auto candidate = chunk_start + i;
        if (has_valid_frame_sequence_at(cursor, candidate, upper_bound))
            return candidate;
    }
    return {};
}

static Optional<MediaStream::ByteRange> find_containing_byte_range(Vector<MediaStream::ByteRange> const& byte_ranges, size_t byte_position)
{
    auto index = lower_bound_index(byte_ranges, byte_position, compare_byte_range_end_to_position);
    if (index >= byte_ranges.size() || byte_position < byte_ranges[index].start)
        return {};
    return byte_ranges[index];
}

static void scan_available_frames_for_cached_range(MediaStreamCursor& cursor, MP3Navigator::CachedRange& range, MediaStream::ByteRange const& byte_range)
{
    auto position = max(range.last_scanned_byte, range.byte_start);
    auto upper_bound = byte_range.end;

    while (position + 4 <= upper_bound) {
        if (!seek(cursor, position))
            return;
        FrameInfo frame_info;
        if (!parse_frame_header(cursor, frame_info))
            return;
        if (position + frame_info.frame_byte_size > upper_bound)
            return;
        position += frame_info.frame_byte_size;
        range.duration_in_ticks += frame_info.duration_in_ticks;
        range.last_scanned_byte = position;
    }
}

static ByteTimePoint scanned_endpoint(MP3Navigator::CachedRange const& range)
{
    return {
        max(range.last_scanned_byte, range.byte_start),
        range.time_start + duration_from_ticks(range.duration_in_ticks),
    };
}

static Optional<ByteTimePoint> file_endpoint_after(ByteTimePoint const& left, Optional<u64> file_size, AK::Duration total_duration)
{
    if (!file_size.has_value())
        return {};
    if (*file_size <= left.byte)
        return {};
    if (total_duration <= left.time)
        return {};
    return ByteTimePoint { static_cast<size_t>(*file_size), total_duration };
}

static EnclosingCachedRanges find_enclosing_cached_ranges_for_timestamp(Vector<MP3Navigator::CachedRange> const& cached_ranges, size_t first_frame_position, AK::Duration timestamp)
{
    EnclosingCachedRanges ranges {
        MP3Navigator::CachedRange {
            .byte_start = first_frame_position,
            .last_scanned_byte = first_frame_position,
        },
        {},
    };
    for (auto const& range : cached_ranges) {
        if (range.time_start <= timestamp) {
            ranges.before = range;
            continue;
        }
        ranges.after = range;
        break;
    }
    return ranges;
}

static AK::Duration estimate_time_for_cached_range_start(Vector<MP3Navigator::CachedRange> const& cached_ranges, size_t byte_start, size_t first_frame_position, Optional<u64> file_size, AK::Duration total_duration)
{
    ByteTimePoint left { first_frame_position, AK::Duration::zero() };
    for (auto const& range : cached_ranges) {
        if (range.byte_start >= byte_start)
            break;
        left = scanned_endpoint(range);
    }

    auto right = file_endpoint_after(left, file_size, total_duration);
    if (!right.has_value())
        return left.time;
    return interpolate_timestamp_at_byte(byte_start, left, *right);
}

static bool insert_cached_range_sorted(Vector<MP3Navigator::CachedRange>& cached_ranges, MP3Navigator::CachedRange new_range)
{
    size_t insert_index = cached_ranges.size();
    for (size_t i = 0; i < cached_ranges.size(); i++) {
        if (cached_ranges[i].byte_start == new_range.byte_start)
            return false;
        if (cached_ranges[i].byte_start > new_range.byte_start) {
            insert_index = i;
            break;
        }
    }

    VERIFY(insert_index == 0 || cached_ranges[insert_index - 1].byte_start < new_range.byte_start);
    VERIFY(insert_index == cached_ranges.size() || new_range.byte_start < cached_ranges[insert_index].byte_start);
    cached_ranges.insert(insert_index, new_range);
    return true;
}

static FrameScanResult scan_available_seek_frames(MediaStreamCursor& cursor, MP3Navigator::CachedRange const& range, Optional<MP3Navigator::CachedRange> const& next_range, AK::Duration target)
{
    VERIFY(range.time_start <= target);

    auto target_duration_in_ticks = (target - range.time_start).to_time_units(1, EXACT_MPEG_AUDIO_DURATION_TIMEBASE);
    auto scan_limit_byte = NumericLimits<size_t>::max();
    if (next_range.has_value()) {
        VERIFY(range.byte_start < next_range->byte_start);
        VERIFY(range.time_start <= next_range->time_start);

        scan_limit_byte = next_range->byte_start;
    }

    auto position = range.byte_start;
    auto last_frame_position = position;
    auto last_frame_duration_in_ticks = 0ull;
    u64 duration_in_ticks = 0;
    while (position + 4 <= scan_limit_byte) {
        if (!seek(cursor, position))
            break;
        FrameInfo frame_info;
        if (!parse_frame_header(cursor, frame_info))
            break;
        if (static_cast<i64>(duration_in_ticks + frame_info.duration_in_ticks) > target_duration_in_ticks) {
            auto frame_time = range.time_start + duration_from_ticks(duration_in_ticks);
            return {
                { position, frame_time },
                { last_frame_position, range.time_start + duration_from_ticks(last_frame_duration_in_ticks) },
                SeekedPosition { static_cast<i64>(position), frame_time },
            };
        }
        last_frame_position = position;
        last_frame_duration_in_ticks = duration_in_ticks;
        position += frame_info.frame_byte_size;
        duration_in_ticks += frame_info.duration_in_ticks;
    }

    return {
        { position, range.time_start + duration_from_ticks(duration_in_ticks) },
        { last_frame_position, range.time_start + duration_from_ticks(last_frame_duration_in_ticks) },
        {},
    };
}

static void scan_cached_range(MediaStreamCursor& cursor, MP3Navigator::CachedRange& range, MediaStream::ByteRange const& byte_range)
{
    if (range.last_scanned_byte > byte_range.end) {
        range.last_scanned_byte = range.byte_start;
        range.duration_in_ticks = 0;
    } else if (range.last_scanned_byte == byte_range.end) {
        return;
    }
    scan_available_frames_for_cached_range(cursor, range, byte_range);
}

static bool insert_cached_range_for_byte_range(MediaStreamCursor& cursor, Vector<MP3Navigator::CachedRange>& ranges, MediaStream::ByteRange const& byte_range, size_t first_frame_position, Optional<u64> file_size, AK::Duration total_duration)
{
    if (byte_range.end <= first_frame_position)
        return false;

    auto search_start = max(byte_range.start, first_frame_position);
    if (search_start >= byte_range.end)
        return false;

    auto frame_boundary = find_frame_boundary_at_or_after(cursor, search_start, byte_range.end);
    if (!frame_boundary.has_value())
        return false;

    MP3Navigator::CachedRange range {
        .byte_start = *frame_boundary,
        .time_start = estimate_time_for_cached_range_start(ranges, *frame_boundary, first_frame_position, file_size, total_duration),
        .last_scanned_byte = *frame_boundary,
        .duration_in_ticks = 0,
    };
    scan_available_frames_for_cached_range(cursor, range, byte_range);

    return insert_cached_range_sorted(ranges, range);
}

MP3Navigator::MP3Navigator(NonnullRefPtr<MediaStream> stream, size_t first_frame_position, AK::Duration total_duration)
    : m_stream(move(stream))
    , m_first_frame_position(first_frame_position)
    , m_total_duration(total_duration)
    , m_buffered_range_scanning_cursor(m_stream->create_cursor())
    , m_seek_range_scanning_cursor(m_stream->create_cursor())
    , m_seek_cursor(m_stream->create_cursor())
{
    m_buffered_range_scanning_cursor->set_is_blocking(false);
    m_seek_range_scanning_cursor->set_is_blocking(false);
    m_seek_cursor->set_is_blocking(true);

    m_cached_ranges.append({
        .byte_start = m_first_frame_position,
        .time_start = AK::Duration::zero(),
        .last_scanned_byte = m_first_frame_position,
        .duration_in_ticks = 0,
    });
}

TimeRanges MP3Navigator::buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const
{
    Sync::MutexLocker locker { m_mutex };
    update_cached_ranges(byte_ranges, *m_buffered_range_scanning_cursor);

    TimeRanges result;
    auto file_size = m_stream->expected_size();
    for (auto const& range : m_cached_ranges) {
        auto byte_range = find_containing_byte_range(byte_ranges, range.byte_start);
        if (!byte_range.has_value())
            continue;
        auto time_start = max(range.time_start, AK::Duration::zero());
        auto time_end = range.time_start + duration_from_ticks(range.duration_in_ticks);
        if (file_size.has_value() && byte_range->end == *file_size)
            time_end = AK::Duration::max();
        result.add_range(time_start, time_end);
    }
    return result;
}

DecoderErrorOr<SeekResult> MP3Navigator::seek_to_timestamp(AK::Duration timestamp) const
{
    if (m_total_duration <= AK::Duration::zero())
        return SeekSkipped {};

    timestamp = max(timestamp, AK::Duration::zero());

    EnclosingCachedRanges enclosing_cached_ranges;
    FrameScanResult scan_result;
    {
        Sync::MutexLocker locker { m_mutex };
        update_cached_ranges(m_stream->available_byte_ranges(), *m_seek_range_scanning_cursor);
        enclosing_cached_ranges = find_enclosing_cached_ranges_for_timestamp(m_cached_ranges, m_first_frame_position, timestamp);
        scan_result = scan_available_seek_frames(*m_seek_range_scanning_cursor, enclosing_cached_ranges.before, enclosing_cached_ranges.after, timestamp);
        if (scan_result.frame_spanning_target.has_value())
            return *scan_result.frame_spanning_target;
    }

    auto file_size = m_stream->expected_size();
    Optional<ByteTimePoint> boundary_after_target;
    if (enclosing_cached_ranges.after.has_value())
        boundary_after_target = ByteTimePoint { enclosing_cached_ranges.after->byte_start, enclosing_cached_ranges.after->time_start };
    else
        boundary_after_target = file_endpoint_after(scan_result.scanned_end, file_size, m_total_duration);
    if (!boundary_after_target.has_value())
        return SeekedPosition { static_cast<i64>(scan_result.last_parsed_frame.byte), scan_result.last_parsed_frame.time };

    auto estimated_byte = estimate_byte_for_timestamp(timestamp, scan_result.scanned_end, *boundary_after_target);

    auto resynced_frame_byte = find_frame_boundary_at_or_before(*m_seek_cursor, estimated_byte, m_first_frame_position, boundary_after_target->byte);
    if (!resynced_frame_byte.has_value() && estimated_byte < boundary_after_target->byte)
        resynced_frame_byte = find_frame_boundary_at_or_after(*m_seek_cursor, estimated_byte, boundary_after_target->byte);
    if (!resynced_frame_byte.has_value())
        return DecoderError::format(DecoderErrorCategory::Corrupted, "MP3 resync failed");

    if (*resynced_frame_byte <= scan_result.scanned_end.byte)
        return SeekedPosition { static_cast<i64>(scan_result.last_parsed_frame.byte), scan_result.last_parsed_frame.time };

    auto resolved_time = interpolate_timestamp_at_byte(*resynced_frame_byte, scan_result.scanned_end, *boundary_after_target);
    return SeekedPosition { static_cast<i64>(*resynced_frame_byte), resolved_time };
}

void MP3Navigator::update_cached_ranges(Vector<MediaStream::ByteRange> const& byte_ranges, MediaStreamCursor& cursor) const
{
    if (byte_ranges.is_empty()) {
        m_cached_ranges.clear();
        m_cached_ranges.append({
            .byte_start = m_first_frame_position,
            .time_start = AK::Duration::zero(),
            .last_scanned_byte = m_first_frame_position,
            .duration_in_ticks = 0,
        });
        return;
    }

    auto file_size = m_stream->expected_size();
    size_t cached_index = 0;
    size_t byte_index = 0;

    while (byte_index < byte_ranges.size()) {
        auto const& byte_range = byte_ranges[byte_index];
        VERIFY(byte_range.start < byte_range.end);

        if (cached_index < m_cached_ranges.size() && m_cached_ranges[cached_index].byte_start < byte_range.start) {
            m_cached_ranges.remove(cached_index);
            continue;
        }

        if (cached_index >= m_cached_ranges.size() || m_cached_ranges[cached_index].byte_start >= byte_range.end) {
            if (insert_cached_range_for_byte_range(cursor, m_cached_ranges, byte_range, m_first_frame_position, file_size, m_total_duration))
                cached_index++;
            byte_index++;
            continue;
        }

        auto& cached_range = m_cached_ranges[cached_index];
        VERIFY(byte_range.start <= cached_range.byte_start);
        VERIFY(cached_range.byte_start < byte_range.end);

        scan_cached_range(cursor, cached_range, byte_range);

        while (cached_index + 1 < m_cached_ranges.size() && m_cached_ranges[cached_index + 1].byte_start < byte_range.end)
            m_cached_ranges.remove(cached_index + 1);

        cached_index++;
        byte_index++;
    }

    if (cached_index < m_cached_ranges.size())
        m_cached_ranges.remove(cached_index, m_cached_ranges.size() - cached_index);

    reproject_cached_range_times();
}

void MP3Navigator::reproject_cached_range_times() const
{
    if (m_cached_ranges.size() < 2)
        return;

    auto file_size = m_stream->expected_size();
    for (size_t i = 1; i < m_cached_ranges.size(); i++) {
        auto left = scanned_endpoint(m_cached_ranges[i - 1]);
        auto right = file_endpoint_after(left, file_size, m_total_duration);
        if (!right.has_value()) {
            m_cached_ranges[i].time_start = left.time;
            continue;
        }

        auto projected_time = interpolate_timestamp_at_byte(m_cached_ranges[i].byte_start, left, *right);
        m_cached_ranges[i].time_start = projected_time;
    }
}

}
