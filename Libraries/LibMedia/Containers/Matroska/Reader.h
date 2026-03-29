/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
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
#include "SampleIterator.h"
#include "Streamer.h"

namespace Media::Matroska {

enum class ElementIterationDecision : u8 {
    Continue,
    BreakHere,
    BreakAtEnd,
    EndOfElement,
};

class MEDIA_API Reader {
public:
    typedef Function<DecoderErrorOr<IterationDecision>(TrackEntry const&)> TrackEntryCallback;

    static DecoderErrorOr<Reader> from_stream(NonnullRefPtr<MediaStreamCursor> const&);

    static bool is_matroska_or_webm(NonnullRefPtr<MediaStreamCursor> const&);

    static DecoderErrorOr<size_t> parse_master_element(Streamer&, StringView element_name, Function<DecoderErrorOr<ElementIterationDecision>(u64)> element_consumer);
    static DecoderErrorOr<EBMLHeader> parse_ebml_header(Streamer&, ElementIterationDecision complete_decision = ElementIterationDecision::BreakAtEnd);
    static DecoderErrorOr<SegmentInformation> parse_segment_information_element(Streamer&);
    static DecoderErrorOr<NonnullRefPtr<TrackEntry>> parse_track_entry(Streamer&);
    static DecoderErrorOr<Cluster> parse_cluster_element(Streamer&, u64 timestamp_scale);
    static DecoderErrorOr<Block> parse_simple_block(Streamer&, AK::Duration cluster_timestamp, u64 segment_timestamp_scale, TrackBlockContexts const&);
    static DecoderErrorOr<Block> parse_block_group(Streamer&, AK::Duration cluster_timestamp, u64 segment_timestamp_scale, TrackBlockContexts const&);

    Optional<AK::Duration> duration() const { return m_segment_information.duration(); }

    DecoderErrorOr<void> for_each_track(TrackEntryCallback);
    DecoderErrorOr<void> for_each_track_of_type(TrackEntry::TrackType, TrackEntryCallback);
    DecoderErrorOr<NonnullRefPtr<TrackEntry>> track_for_track_number(u64);
    DecoderErrorOr<size_t> track_count();

    DecoderErrorOr<SampleIterator> create_sample_iterator(NonnullRefPtr<MediaStreamCursor> const& stream_consumer, u64 track_number);
    DecoderErrorOr<SampleIterator> seek_to_random_access_point(SampleIterator, AK::Duration);

    Optional<Vector<TrackCuePoint> const&> cue_points_for_track(u64 track_number);

    static size_t find_cue_point_index_at_or_before(Vector<TrackCuePoint> const&, Optional<AK::Duration> total_duration, AK::Duration target);

private:
    Reader() = default;

    DecoderErrorOr<void> parse_initial_data(Streamer&);

    DecoderErrorOr<Optional<size_t>> find_first_top_level_element_with_id(Streamer&, StringView element_name, u32 element_id);

    DecoderErrorOr<void> parse_segment_information(Streamer&);

    DecoderErrorOr<void> parse_tracks(Streamer&);
    void fix_track_quirks();
    void fix_ffmpeg_webm_quirk();

    DecoderErrorOr<void> parse_cues(Streamer&);

    bool has_cues_for_track(u64 track_number);
    DecoderErrorOr<void> seek_to_cue_for_timestamp(SampleIterator&, AK::Duration const&, Vector<TrackCuePoint> const&, CuePointTarget);

    Optional<EBMLHeader> m_header;

    size_t m_segment_contents_position { 0 };
    Optional<size_t> m_segment_contents_size { 0 };

    HashMap<u32, size_t> m_seek_entries;
    size_t m_last_top_level_element_position { 0 };

    SegmentInformation m_segment_information;

    OrderedHashMap<u64, NonnullRefPtr<TrackEntry>> m_tracks;

    size_t m_first_cluster_position { 0 };

    // The vectors must be sorted by timestamp at all times.
    HashMap<u64, Vector<TrackCuePoint>> m_cues;
};

}
