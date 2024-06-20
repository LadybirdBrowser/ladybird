/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <Zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntegralMath.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibCore/MappedFile.h>
#include <LibMedia/DecoderError.h>

#include "Document.h"

namespace Media::Matroska {

class SampleIterator;
class Streamer;

class Reader {
public:
    typedef Function<DecoderErrorOr<IterationDecision>(TrackEntry const&)> TrackEntryCallback;

    static DecoderErrorOr<Reader> from_file(StringView path);
    static DecoderErrorOr<Reader> from_mapped_file(NonnullOwnPtr<Core::MappedFile> mapped_file);

    static DecoderErrorOr<Reader> from_data(ReadonlyBytes data);

    EBMLHeader const& header() const { return m_header.value(); }

    DecoderErrorOr<SegmentInformation> segment_information();

    DecoderErrorOr<void> for_each_track(TrackEntryCallback);
    DecoderErrorOr<void> for_each_track_of_type(TrackEntry::TrackType, TrackEntryCallback);
    DecoderErrorOr<NonnullRefPtr<TrackEntry>> track_for_track_number(u64);
    DecoderErrorOr<size_t> track_count();

    DecoderErrorOr<SampleIterator> create_sample_iterator(u64 track_number);
    DecoderErrorOr<SampleIterator> seek_to_random_access_point(SampleIterator, Duration);
    DecoderErrorOr<Optional<Vector<CuePoint> const&>> cue_points_for_track(u64 track_number);
    DecoderErrorOr<bool> has_cues_for_track(u64 track_number);

private:
    Reader(ReadonlyBytes data)
        : m_data(data)
    {
    }

    DecoderErrorOr<void> parse_initial_data();

    DecoderErrorOr<Optional<size_t>> find_first_top_level_element_with_id([[maybe_unused]] StringView element_name, u32 element_id);

    DecoderErrorOr<void> ensure_tracks_are_parsed();
    DecoderErrorOr<void> parse_tracks(Streamer&);

    DecoderErrorOr<void> parse_cues(Streamer&);
    DecoderErrorOr<void> ensure_cues_are_parsed();
    DecoderErrorOr<void> seek_to_cue_for_timestamp(SampleIterator&, Duration const&);

    RefPtr<Core::SharedMappedFile> m_mapped_file;
    ReadonlyBytes m_data;

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

class SampleIterator {
public:
    DecoderErrorOr<Block> next_block();
    Cluster const& current_cluster() const { return *m_current_cluster; }
    Optional<Duration> const& last_timestamp() const { return m_last_timestamp; }
    TrackEntry const& track() const { return *m_track; }

private:
    friend class Reader;

    SampleIterator(RefPtr<Core::SharedMappedFile> file, ReadonlyBytes data, TrackEntry& track, u64 timestamp_scale, size_t position)
        : m_file(move(file))
        , m_data(data)
        , m_track(track)
        , m_segment_timestamp_scale(timestamp_scale)
        , m_position(position)
    {
    }

    DecoderErrorOr<void> seek_to_cue_point(CuePoint const& cue_point);

    RefPtr<Core::SharedMappedFile> m_file;
    ReadonlyBytes m_data;
    NonnullRefPtr<TrackEntry> m_track;
    u64 m_segment_timestamp_scale { 0 };

    // Must always point to an element ID or the end of the stream.
    size_t m_position { 0 };

    Optional<Duration> m_last_timestamp;

    Optional<Cluster> m_current_cluster;
};

class Streamer {
public:
    Streamer(ReadonlyBytes data)
        : m_data(data)
    {
    }

    u8 const* data() { return m_data.data() + m_position; }

    char const* data_as_chars() { return reinterpret_cast<char const*>(data()); }

    size_t octets_read() { return m_octets_read.last(); }

    void push_octets_read() { m_octets_read.append(0); }

    void pop_octets_read()
    {
        auto popped = m_octets_read.take_last();
        if (!m_octets_read.is_empty())
            m_octets_read.last() += popped;
    }

    ErrorOr<u8> read_octet();

    ErrorOr<i16> read_i16();

    ErrorOr<u64> read_variable_size_integer(bool mask_length = true);
    ErrorOr<i64> read_variable_size_signed_integer();

    ErrorOr<u64> read_u64();
    ErrorOr<double> read_float();

    ErrorOr<ByteString> read_string();

    ErrorOr<void> read_unknown_element();

    ErrorOr<ReadonlyBytes> read_raw_octets(size_t num_octets);

    size_t position() const { return m_position; }
    size_t remaining() const { return m_data.size() - position(); }

    bool at_end() const { return remaining() == 0; }
    bool has_octet() const { return remaining() >= 1; }

    ErrorOr<void> seek_to_position(size_t position);

private:
    ReadonlyBytes m_data;
    size_t m_position { 0 };
    Vector<size_t> m_octets_read { 0 };
};

}
