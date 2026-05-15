/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/ReadonlyBytesCursor.h>

#include "FLACNavigator.h"

namespace Media {

OwnPtr<FLACNavigator> FLACNavigator::create(ReadonlyBytes first_frame, NonnullRefPtr<MediaStreamCursor> cursor, u32 sample_rate)
{
    if (first_frame.size() < 2)
        return {};
    u16 sync_code = (static_cast<u16>(first_frame[0]) << 8) | first_frame[1];
    if (!Codecs::FLAC::is_sync_code(sync_code))
        return {};

    bool is_fixed_blocksize = (sync_code & 1) == 0;

    auto frame_cursor = make_ref_counted<ReadonlyBytesCursor>(first_frame);
    auto navigator = adopt_own(*new (nothrow) FLACNavigator(move(cursor), sample_rate, sync_code, 0));
    auto frame_info = Codecs::FLAC::parse_frame_header(frame_cursor, sync_code, 0);
    if (!frame_info.has_value())
        return {};

    if (is_fixed_blocksize)
        navigator->m_fixed_block_size = frame_info->block_size;

    return navigator;
}

static constexpr size_t SCAN_CHUNK_SIZE = 4096;

Optional<Codecs::FLAC::FrameInfo> FLACNavigator::find_first_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const
{
    VERIFY(search_start <= search_end);

    Array<u8, SCAN_CHUNK_SIZE> chunk;
    auto chunk_start = search_start;

    while (chunk_start < search_end) {
        if (cursor.seek_to_position(chunk_start).is_error())
            return {};
        auto read_result = cursor.read_into(chunk);
        if (read_result.is_error())
            return {};
        auto bytes_read = read_result.value();
        if (bytes_read < 2)
            return {};

        for (size_t i = 0; i + 1 < bytes_read; i++) {
            if (chunk[i] != 0xFF)
                continue;
            u16 maybe_sync_code = (static_cast<u16>(chunk[i]) << 8) | chunk[i + 1];
            if (maybe_sync_code != m_sync_code)
                continue;

            auto sync_position = chunk_start + i;
            if (cursor.seek_to_position(sync_position).is_error())
                return {};
            auto frame_info = Codecs::FLAC::parse_frame_header(cursor, m_sync_code, m_fixed_block_size);
            if (frame_info.has_value())
                return frame_info;
        }

        chunk_start += bytes_read - 1;
    }
    return {};
}

Optional<Codecs::FLAC::FrameInfo> FLACNavigator::find_last_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const
{
    VERIFY(search_start <= search_end);

    Array<u8, SCAN_CHUNK_SIZE> chunk;
    auto chunk_end = search_end;

    while (chunk_end > search_start) {
        auto chunk_start = chunk_end > SCAN_CHUNK_SIZE ? chunk_end - SCAN_CHUNK_SIZE : 0;
        chunk_start = max(chunk_start, search_start);
        VERIFY(chunk_start <= chunk_end);
        auto chunk_size = chunk_end - chunk_start;
        if (chunk_size < 2)
            return {};

        if (cursor.seek_to_position(chunk_start).is_error())
            return {};
        if (cursor.read_until_filled(chunk.span().trim(chunk_size)).is_error())
            return {};

        for (size_t i = chunk_size - 1; i-- > 0;) {
            if (chunk[i] != 0xFF)
                continue;
            u16 maybe_sync_code = (static_cast<u16>(chunk[i]) << 8) | chunk[i + 1];
            if (maybe_sync_code != m_sync_code)
                continue;

            auto sync_position = chunk_start + i;
            if (cursor.seek_to_position(sync_position).is_error())
                return {};
            auto frame_info = Codecs::FLAC::parse_frame_header(cursor, m_sync_code, m_fixed_block_size);
            if (frame_info.has_value())
                return frame_info;
        }

        chunk_end = chunk_start + 1;
    }

    return {};
}

AK::Duration FLACNavigator::sample_to_duration(u64 sample) const
{
    return AK::Duration::from_time_units(static_cast<i64>(sample), 1, m_sample_rate);
}

Optional<AK::Duration> FLACNavigator::scan_forward_for_timestamp(size_t search_start, size_t search_end) const
{
    auto first = find_first_frame(*m_cursor, search_start, search_end);
    if (!first.has_value())
        return {};
    return sample_to_duration(first->sample_number);
}

Optional<AK::Duration> FLACNavigator::scan_backward_for_end_timestamp(size_t search_start, size_t search_end) const
{
    auto last = find_last_frame(*m_cursor, search_start, search_end);
    if (!last.has_value())
        return {};
    return sample_to_duration(last->sample_number + last->block_size);
}

void FLACNavigator::on_cached_range_changed(FLACCachedRange& cached, CachedRangeChange change) const
{
    bool rescan_start = has_flag(change, CachedRangeChange::Start);
    bool rescan_end = has_flag(change, CachedRangeChange::End);

    if (rescan_start && !cached.time_end.has_value())
        rescan_end = true;
    if (rescan_end && !cached.time_start.has_value())
        rescan_start = true;

    if (rescan_start)
        cached.time_start = scan_forward_for_timestamp(cached.byte_start, cached.byte_end);
    if (rescan_end)
        cached.time_end = scan_backward_for_end_timestamp(cached.byte_start, cached.byte_end);
}

void FLACNavigator::append_time_range(FLACCachedRange const& cached_range, TimeRanges& to)
{
    if (!cached_range.time_start.has_value() || !cached_range.time_end.has_value())
        return;

    auto time_start = max(cached_range.time_start.value(), AK::Duration::zero());
    auto time_end = cached_range.time_end.value();
    if (time_start >= time_end)
        return;

    to.add_range(time_start, time_end);
}

}
