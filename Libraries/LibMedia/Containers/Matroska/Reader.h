/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <Zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibCore/MappedFile.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

#include "Document.h"

namespace Media::Matroska {

class SampleIterator;
class Streamer;

class MEDIA_API Reader {
public:
    typedef Function<DecoderErrorOr<IterationDecision>(TrackEntry const&)> TrackEntryCallback;

    static DecoderErrorOr<Reader> from_stream(IncrementallyPopulatedStream::Cursor&);

    EBMLHeader const& header() const { return m_header.value(); }

    DecoderErrorOr<SegmentInformation> segment_information();

    DecoderErrorOr<void> for_each_track(TrackEntryCallback);
    DecoderErrorOr<void> for_each_track_of_type(TrackEntry::TrackType, TrackEntryCallback);
    DecoderErrorOr<NonnullRefPtr<TrackEntry>> track_for_track_number(u64);
    DecoderErrorOr<size_t> track_count();

    DecoderErrorOr<SampleIterator> create_sample_iterator(NonnullRefPtr<IncrementallyPopulatedStream::Cursor> const& stream_consumer, u64 track_number);
    DecoderErrorOr<SampleIterator> seek_to_random_access_point(SampleIterator, AK::Duration);
    DecoderErrorOr<Optional<Vector<CuePoint> const&>> cue_points_for_track(u64 track_number);
    DecoderErrorOr<bool> has_cues_for_track(u64 track_number);

private:
    Reader(IncrementallyPopulatedStream::Cursor& stream_cursor)
        : m_stream_cursor(stream_cursor)
    {
    }

    DecoderErrorOr<void> parse_initial_data();

    DecoderErrorOr<Optional<size_t>> find_first_top_level_element_with_id([[maybe_unused]] StringView element_name, u32 element_id);

    DecoderErrorOr<void> ensure_tracks_are_parsed();
    DecoderErrorOr<void> parse_tracks(Streamer&);
    void fix_track_quirks();
    void fix_ffmpeg_webm_quirk();

    DecoderErrorOr<void> parse_cues(Streamer&);
    DecoderErrorOr<void> ensure_cues_are_parsed();
    DecoderErrorOr<void> seek_to_cue_for_timestamp(SampleIterator&, AK::Duration const&);

    NonnullRefPtr<IncrementallyPopulatedStream::Cursor> m_stream_cursor;

    Optional<EBMLHeader> m_header;

    size_t m_segment_contents_position { 0 };
    size_t m_segment_contents_size { 0 };

    HashMap<u32, size_t> m_seek_entries;
    size_t m_last_top_level_element_position { 0 };

    Optional<SegmentInformation> m_segment_information;

    OrderedHashMap<u64, NonnullRefPtr<TrackEntry>> m_tracks;

    // The vectors must be sorted by timestamp at all times.
    HashMap<u64, Vector<CuePoint>> m_cues;
    bool m_cues_have_been_parsed { false };
};

class MEDIA_API SampleIterator {
public:
    DecoderErrorOr<Block> next_block();
    Cluster const& current_cluster() const { return *m_current_cluster; }
    Optional<AK::Duration> const& last_timestamp() const { return m_last_timestamp; }
    TrackEntry const& track() const { return *m_track; }

private:
    friend class Reader;

    SampleIterator(NonnullRefPtr<IncrementallyPopulatedStream::Cursor> const& stream_cursor, TrackEntry& track, u64 timestamp_scale, size_t segment_contents_position, size_t position)
        : m_stream_cursor(stream_cursor)
        , m_track(track)
        , m_segment_timestamp_scale(timestamp_scale)
        , m_segment_contents_position(segment_contents_position)
        , m_position(position)
    {
    }

    DecoderErrorOr<void> seek_to_cue_point(CuePoint const& cue_point);

    NonnullRefPtr<IncrementallyPopulatedStream::Cursor> m_stream_cursor;
    NonnullRefPtr<TrackEntry> m_track;
    u64 m_segment_timestamp_scale { 0 };
    size_t m_segment_contents_position { 0 };

    // Must always point to an element ID or the end of the stream.
    size_t m_position { 0 };

    Optional<AK::Duration> m_last_timestamp;

    Optional<Cluster> m_current_cluster;
};

class Streamer {
public:
    Streamer(NonnullRefPtr<IncrementallyPopulatedStream::Cursor> const& stream_cursor)
        : m_stream_cursor(stream_cursor)
    {
    }

    size_t octets_read() { return m_octets_read.last(); }

    void push_octets_read() { m_octets_read.append(0); }

    void pop_octets_read()
    {
        auto popped = m_octets_read.take_last();
        if (!m_octets_read.is_empty())
            m_octets_read.last() += popped;
    }

    DecoderErrorOr<u8> read_octet();

    DecoderErrorOr<i16> read_i16();

    DecoderErrorOr<u64> read_variable_size_integer(bool mask_length = true);
    DecoderErrorOr<i64> read_variable_size_signed_integer();

    DecoderErrorOr<u64> read_u64();
    DecoderErrorOr<double> read_float();

    DecoderErrorOr<String> read_string();

    DecoderErrorOr<void> read_unknown_element();

    DecoderErrorOr<ByteBuffer> read_raw_octets(size_t num_octets);

    size_t position() const { return m_stream_cursor->position(); }

    DecoderErrorOr<void> seek_to_position(size_t position);

private:
    NonnullRefPtr<IncrementallyPopulatedStream::Cursor> m_stream_cursor;
    Vector<size_t> m_octets_read { 0 };
};

}
