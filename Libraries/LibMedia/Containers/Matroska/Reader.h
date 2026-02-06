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
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

#include "Document.h"

namespace Media::Matroska {

class SampleIterator;
class Streamer;

struct TrackCuePoint {
    AK::Duration timestamp;
    CueTrackPosition position;
};

enum class CuePointTarget : u8 {
    Cluster,
    Block,
};

class MEDIA_API Reader {
public:
    typedef Function<DecoderErrorOr<IterationDecision>(TrackEntry const&)> TrackEntryCallback;

    static DecoderErrorOr<Reader> from_stream(NonnullRefPtr<MediaStreamCursor> const&);

    static bool is_matroska_or_webm(NonnullRefPtr<MediaStreamCursor> const&);

    Optional<AK::Duration> duration() { return m_segment_information.duration(); }

    DecoderErrorOr<void> for_each_track(TrackEntryCallback);
    DecoderErrorOr<void> for_each_track_of_type(TrackEntry::TrackType, TrackEntryCallback);
    DecoderErrorOr<NonnullRefPtr<TrackEntry>> track_for_track_number(u64);
    DecoderErrorOr<size_t> track_count();

    DecoderErrorOr<SampleIterator> create_sample_iterator(NonnullRefPtr<MediaStreamCursor> const& stream_consumer, u64 track_number);
    DecoderErrorOr<SampleIterator> seek_to_random_access_point(SampleIterator, AK::Duration);

private:
    Reader() = default;

    DecoderErrorOr<void> parse_initial_data(Streamer&);

    DecoderErrorOr<Optional<size_t>> find_first_top_level_element_with_id(Streamer&, StringView element_name, u32 element_id);

    DecoderErrorOr<void> parse_segment_information(Streamer&);

    DecoderErrorOr<void> parse_tracks(Streamer&);
    void fix_track_quirks();
    void fix_ffmpeg_webm_quirk();

    DecoderErrorOr<void> parse_cues(Streamer&);

    Optional<Vector<TrackCuePoint> const&> cue_points_for_track(u64 track_number);
    bool has_cues_for_track(u64 track_number);
    DecoderErrorOr<void> seek_to_cue_for_timestamp(SampleIterator&, AK::Duration const&, Vector<TrackCuePoint> const&, CuePointTarget);

    Optional<EBMLHeader> m_header;

    size_t m_segment_contents_position { 0 };
    size_t m_segment_contents_size { 0 };

    HashMap<u32, size_t> m_seek_entries;
    size_t m_last_top_level_element_position { 0 };

    SegmentInformation m_segment_information;

    OrderedHashMap<u64, NonnullRefPtr<TrackEntry>> m_tracks;

    size_t m_first_cluster_position { 0 };

    // The vectors must be sorted by timestamp at all times.
    HashMap<u64, Vector<TrackCuePoint>> m_cues;
};

class MEDIA_API SampleIterator {
    AK_MAKE_DEFAULT_MOVABLE(SampleIterator);
    AK_MAKE_DEFAULT_COPYABLE(SampleIterator);

public:
    ~SampleIterator();

    DecoderErrorOr<Block> next_block();
    DecoderErrorOr<Vector<ByteBuffer>> get_frames(Block);
    Cluster const& current_cluster() const { return *m_current_cluster; }
    Optional<AK::Duration> const& last_timestamp() const { return m_last_timestamp; }
    TrackEntry const& track() const { return *m_track; }
    MediaStreamCursor& cursor() { return m_stream_cursor; }

private:
    friend class Reader;

    SampleIterator(NonnullRefPtr<MediaStreamCursor> const& stream_cursor, TrackEntry& track, u64 timestamp_scale, size_t segment_contents_position, size_t position);

    DecoderErrorOr<void> seek_to_cue_point(TrackCuePoint const& cue_point, CuePointTarget);

    NonnullRefPtr<MediaStreamCursor> m_stream_cursor;
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
    Streamer(NonnullRefPtr<MediaStreamCursor> const& stream_cursor);
    ~Streamer();

    DecoderErrorOr<u8> read_octet();

    DecoderErrorOr<i16> read_i16();

    DecoderErrorOr<u64> read_variable_size_integer(bool mask_length = true);
    DecoderErrorOr<i64> read_variable_size_signed_integer();

    DecoderErrorOr<u64> read_u64();
    DecoderErrorOr<double> read_float();

    DecoderErrorOr<String> read_string();

    DecoderErrorOr<void> read_unknown_element();

    DecoderErrorOr<ByteBuffer> read_raw_octets(size_t num_octets);

    size_t position() const;

    DecoderErrorOr<void> seek_to_position(size_t position);

private:
    NonnullRefPtr<MediaStreamCursor> m_stream_cursor;
};

}
