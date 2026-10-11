/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SampleIterator.h"
#include "Reader.h"
#include <AK/Checked.h>
#include <AK/SaturatingMath.h>

namespace Media::ISOBMFF {

DecoderErrorOr<SampleTableIterator> SampleTableIterator::create(TrackEntry const& track)
{
    SampleTableIterator iterator(*track.sample_tables);
    auto const& tables = *iterator.m_tables;
    if (tables.time_to_sample.has_value())
        iterator.m_times = tables.time_to_sample->span();
    if (tables.composition_offsets.has_value())
        iterator.m_compositions = tables.composition_offsets->span();
    if (tables.sample_to_chunk.has_value())
        iterator.m_chunks = tables.sample_to_chunk->span();
    if (tables.chunk_offsets.has_value())
        iterator.m_offsets = tables.chunk_offsets->span();
    if (tables.sync_samples.has_value())
        iterator.m_sync = tables.sync_samples->span();
    if (tables.sample_sizes.has_value())
        iterator.m_sample_count = tables.sample_sizes->sample_count;
    else if (track.sample_table_entry_count != 0)
        return DecoderError::corrupted("Missing sample sizes"sv);
    return iterator;
}

DecoderErrorOr<Optional<Sample>> SampleTableIterator::next(TrackEntry const& track)
{
    if (m_sample_index == m_sample_count) {
        if (m_time_index != m_times.size() || m_composition_index != m_compositions.size()
            || m_chunk_index != m_offsets.size() || m_chunk_run_index != m_chunks.size() || m_sync_index != m_sync.size())
            return DecoderError::corrupted("Sample tables contain unused entries"sv);
        return Optional<Sample> {};
    }
    if (m_time_remaining == 0) {
        if (m_time_index == m_times.size())
            return DecoderError::corrupted("Sample timing table ended early"sv);
        auto const& entry = m_times[m_time_index++];
        m_time_remaining = entry.sample_count;
        m_delta = entry.sample_delta;
        if (m_time_remaining == 0 || m_time_remaining > m_sample_count - m_sample_index)
            return DecoderError::corrupted("Invalid sample timing run length"sv);
    }
    if (m_tables->composition_offsets.has_value() && m_composition_remaining == 0) {
        if (m_composition_index == m_compositions.size())
            return DecoderError::corrupted("Composition offset table ended early"sv);
        auto const& entry = m_compositions[m_composition_index++];
        m_composition_remaining = entry.sample_count;
        m_composition_offset = entry.sample_offset;
        if (m_composition_remaining == 0 || m_composition_remaining > m_sample_count - m_sample_index)
            return DecoderError::corrupted("Invalid composition offset run length"sv);
    }
    if (m_chunk_remaining == 0) {
        if (m_chunk_index == m_offsets.size())
            return DecoderError::corrupted("Chunk offset table ended early"sv);
        ++m_chunk_index;
        if (m_chunk_index == m_next_chunk_run) {
            if (m_chunk_run_index == m_chunks.size())
                return DecoderError::corrupted("Sample to chunk table ended early"sv);
            auto const& entry = m_chunks[m_chunk_run_index++];
            if (entry.first_chunk != m_chunk_index)
                return DecoderError::corrupted("Invalid first chunk in sample to chunk table"sv);
            m_samples_per_chunk = entry.samples_per_chunk;
            m_description_index = entry.sample_description_index;
            if (m_samples_per_chunk == 0 || !track.sample_entry_for_description_index(m_description_index).has_value())
                return DecoderError::corrupted("Invalid sample to chunk entry"sv);
            m_next_chunk_run = 0;
            if (m_chunk_run_index < m_chunks.size()) {
                m_next_chunk_run = m_chunks[m_chunk_run_index].first_chunk;
                if (m_next_chunk_run <= m_chunk_index || m_next_chunk_run > m_offsets.size())
                    return DecoderError::corrupted("Sample to chunk entries are out of order"sv);
            }
        }
        m_chunk_remaining = m_samples_per_chunk;
        if (m_chunk_remaining > m_sample_count - m_sample_index)
            return DecoderError::corrupted("Chunk contains too many samples"sv);
        auto offset = m_offsets[m_chunk_index - 1];
        if (offset > NumericLimits<i64>::max())
            return DecoderError::corrupted("Chunk offset is too large"sv);
        m_data_position = offset;
    }
    auto size = m_tables->sample_sizes->size_of_sample(m_sample_index);
    if (m_tables->sync_samples.has_value() && m_next_sync_sample <= m_sample_index && m_sync_index < m_sync.size()) {
        auto next = m_sync[m_sync_index++];
        if (next <= m_next_sync_sample || next > m_sample_count)
            return DecoderError::corrupted("Invalid sync sample number"sv);
        m_next_sync_sample = next;
    }
    auto decode_time = saturating_add(m_decode_time, track.composition_to_presentation_offset);
    auto pts = saturating_add(decode_time, m_composition_offset);
    auto presentation_time = AK::Duration::from_time_units(pts, 1, track.timescale);
    Sample sample {
        .track_id = track.track_id,
        .sample_description_index = m_description_index,
        .decode_timestamp = AK::Duration::from_time_units(decode_time, 1, track.timescale),
        .presentation_timestamp = presentation_time,
        .duration = AK::Duration::from_time_units(saturating_add(pts, static_cast<i64>(m_delta)), 1, track.timescale) - presentation_time,
        .is_sync_sample = !m_tables->sync_samples.has_value() || m_next_sync_sample == m_sample_index + 1,
        .data_position = m_data_position,
        .data_size = size,
    };
    Checked<size_t> end = m_data_position;
    end += size;
    if (end.has_overflow() || end.value() > NumericLimits<i64>::max())
        return DecoderError::corrupted("Sample data offset overflow"sv);
    m_data_position = end.value();
    m_decode_time = saturating_add(m_decode_time, static_cast<i64>(m_delta));
    ++m_sample_index;
    --m_time_remaining;
    --m_chunk_remaining;
    if (m_composition_remaining)
        --m_composition_remaining;
    return Optional<Sample> { sample };
}

SampleIterator::SampleIterator(NonnullRefPtr<MediaStreamCursor> cursor, NonnullRefPtr<TrackEntry const> track, SampleTableIterator table, TrackFragmentContexts contexts, size_t movie_end, bool has_fragments)
    : m_cursor(move(cursor))
    , m_track(move(track))
    , m_table(move(table))
    , m_contexts(move(contexts))
    , m_next_box(movie_end)
    , m_finished(!has_fragments)
{
}

DecoderErrorOr<SampleIterator> SampleIterator::create(NonnullRefPtr<MediaStreamCursor> cursor, Movie const& movie, u32 track_id, size_t movie_end)
{
    auto track = movie.tracks.get(track_id);
    if (!track.has_value())
        return DecoderError::corrupted("Unknown track"sv);
    auto table = TRY(SampleTableIterator::create(**track));
    TrackFragmentContexts contexts;
    for (auto const& [id, entry] : movie.tracks)
        contexts.set(id, { entry->timescale, entry->composition_to_presentation_offset, entry->sample_entries.size(), entry->fragment_defaults, AK::clamp_to<i64>(entry->duration.value_or(0)) });
    return SampleIterator(move(cursor), **track, move(table), move(contexts), movie_end, movie.has_movie_extends_box);
}

DecoderErrorOr<Optional<Sample>> SampleIterator::next()
{
    if (m_cursor->is_aborted())
        return DecoderError::with_description(DecoderErrorCategory::Aborted, "Sample iteration aborted"sv);
    Streamer stream(m_cursor);
    if (!m_finished_table) {
        auto table = m_table;
        auto sample = TRY(table.next(*m_track));
        m_table = table;
        if (sample.has_value())
            return sample;
        m_contexts.get(m_track->track_id)->next_decode_time = table.next_decode_time();
        m_finished_table = true;
    }
    while (!m_finished) {
        if (m_cursor->is_aborted())
            return DecoderError::with_description(DecoderErrorCategory::Aborted, "Sample iteration aborted"sv);
        if (m_fragment.has_value() && m_fragment->has_next()) {
            auto sample = m_fragment->peek();
            m_fragment->advance();
            if (sample.track_id == m_track->track_id)
                return Optional<Sample> { sample };
            continue;
        }
        if (m_cursor->size().has_value() && m_next_box >= m_cursor->size().value()) {
            m_finished = true;
            break;
        }
        TRY(stream.seek_to_position(m_next_box));
        auto header = TRY(Reader::read_box_header(stream));
        if (!header.end_position().has_value()) {
            // A box extending to EOF cannot be followed by another fragment.
            m_finished = true;
            break;
        }
        if (header.type == MOVIE_FRAGMENT_BOX) {
            auto fragment = TRY(Reader::parse_movie_fragment_box(stream, header, m_contexts));
            for (auto const& run : fragment.track_runs)
                m_contexts.get(run.track_id)->next_decode_time = run.end_decode_time();
            m_fragment.emplace(move(fragment));
        }
        m_next_box = header.end_position().value();
    }
    return Optional<Sample> {};
}

}
