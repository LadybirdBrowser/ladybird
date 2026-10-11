/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "FragmentSampleIterator.h"
#include "Streamer.h"

namespace Media::ISOBMFF {

// A constant-sized cursor over shared sample tables. Copies are cheap seek bookmarks.
class MEDIA_API SampleTableIterator {
public:
    static DecoderErrorOr<SampleTableIterator> create(TrackEntry const&);
    DecoderErrorOr<Optional<Sample>> next(TrackEntry const&);
    i64 next_decode_time() const { return m_decode_time; }

private:
    explicit SampleTableIterator(NonnullRefPtr<SampleTables const> tables)
        : m_tables(move(tables))
    {
    }
    NonnullRefPtr<SampleTables const> m_tables;
    ReadonlySpan<TimeToSampleEntry> m_times;
    ReadonlySpan<CompositionOffsetEntry> m_compositions;
    ReadonlySpan<SampleToChunkEntry> m_chunks;
    ReadonlySpan<u64> m_offsets;
    ReadonlySpan<u32> m_sync;
    u32 m_sample_count { 0 };
    u32 m_sample_index { 0 };
    u32 m_time_index { 0 };
    u32 m_time_remaining { 0 };
    u32 m_delta { 0 };
    u32 m_composition_index { 0 };
    u32 m_composition_remaining { 0 };
    i64 m_composition_offset { 0 };
    u32 m_chunk_index { 0 };
    u32 m_chunk_run_index { 0 };
    u32 m_chunk_remaining { 0 };
    u32 m_samples_per_chunk { 0 };
    u32 m_description_index { 0 };
    u32 m_next_chunk_run { 1 };
    u32 m_sync_index { 0 };
    u32 m_next_sync_sample { 0 };
    size_t m_data_position { 0 };
    i64 m_decode_time { 0 };
};

// Delivers a track's moov samples followed by its moof samples, in decode order.
class MEDIA_API SampleIterator {
public:
    static DecoderErrorOr<SampleIterator> create(NonnullRefPtr<MediaStreamCursor>, Movie const&, u32 track_id, size_t movie_end);
    DecoderErrorOr<Optional<Sample>> next();
    MediaStreamCursor& cursor() { return *m_cursor; }

private:
    SampleIterator(NonnullRefPtr<MediaStreamCursor>, NonnullRefPtr<TrackEntry const>, SampleTableIterator, TrackFragmentContexts, size_t, bool has_fragments);
    NonnullRefPtr<MediaStreamCursor> m_cursor;
    NonnullRefPtr<TrackEntry const> m_track;
    SampleTableIterator m_table;
    TrackFragmentContexts m_contexts;
    Optional<FragmentSampleIterator> m_fragment;
    size_t m_next_box { 0 };
    bool m_finished_table { false };
    bool m_finished { false };
};

}
